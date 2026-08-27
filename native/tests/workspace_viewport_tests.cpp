#include "apex/app/workspace_ai_spline_controller.hpp"
#include "apex/app/workspace_ai_spline_commands.hpp"
#include "apex/app/workspace_viewport.hpp"
#include "apex/app/workspace_shadow_programs.hpp"
#include "apex/authoring/ai_spline_session.hpp"
#include "apex/formats/acd.hpp"
#include "apex/render/view_axis.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace apex::render;
using apex::app::WorkspaceViewportFrameRequest;
using apex::app::WorkspaceViewportFrameStatus;
using apex::app::WorkspaceViewportPortableCloudOptions;
using apex::app::WorkspaceViewportPortableGrassFrameOptions;
using apex::app::WorkspaceViewportPortableGrassOptions;
using apex::app::WorkspaceViewportPrepareRequest;
using apex::app::WorkspaceViewportStockVulkanSourceFrame;

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t full_mip_count(std::uint32_t width, std::uint32_t height) {
    std::uint32_t dimension = std::max(width, height);
    std::uint32_t levels = 1U;
    while (dimension > 1U) {
        dimension >>= 1U;
        ++levels;
    }
    return levels;
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void appendF32(std::vector<std::uint8_t>& bytes, float value) {
    appendU32(bytes, std::bit_cast<std::uint32_t>(value));
}

apex::formats::AiSpline legacyAiSplineFixture() {
    std::vector<std::uint8_t> bytes;
    appendU32(bytes, 2U);
    appendU32(bytes, 4U);
    appendU32(bytes, 123U);
    for (std::uint32_t index = 0U; index < 4U; ++index) {
        appendF32(bytes, static_cast<float>(index) * 10.0F);
        appendF32(bytes, 0.0F);
        appendF32(bytes, 0.0F);
        appendU32(bytes, index);
        appendF32(bytes, static_cast<float>(index + 1U) * 10.0F);
        appendF32(bytes, 0.25F);
        appendF32(bytes, 0.5F);
    }
    return apex::formats::parseAiSpline(bytes, "legacy-controller.ai");
}

class FakeBuffer final : public Buffer {
public:
    FakeBuffer(BufferDescription description, std::span<const std::byte> bytes,
               std::shared_ptr<std::size_t> live_count, Backend backend)
        : info_({description}), bytes_(bytes.begin(), bytes.end()),
          live_count_(std::move(live_count)), backend_(backend) {
        ++*live_count_;
    }
    ~FakeBuffer() override { --*live_count_; }
    Backend backend() const noexcept override { return backend_; }
    const BufferInfo &info() const noexcept override { return info_; }
    [[nodiscard]] const std::vector<std::byte> &bytes() const noexcept {
        return bytes_;
    }

private:
    BufferInfo info_{};
    std::vector<std::byte> bytes_;
    std::shared_ptr<std::size_t> live_count_;
    Backend backend_ = Backend::Vulkan;
};

class FakeTexture final : public Texture {
public:
    explicit FakeTexture(TextureDescription description, Backend backend)
        : info_({description}), backend_(backend) {}
    Backend backend() const noexcept override { return backend_; }
    const TextureInfo &info() const noexcept override { return info_; }

private:
    TextureInfo info_{};
    Backend backend_ = Backend::Vulkan;
};

class FakeDepth final : public DepthAttachment {
public:
    explicit FakeDepth(DepthAttachmentDescription description, Backend backend)
        : info_({description}), backend_(backend) {}
    Backend backend() const noexcept override { return backend_; }
    const DepthAttachmentInfo &info() const noexcept override { return info_; }

private:
    DepthAttachmentInfo info_{};
    Backend backend_ = Backend::Vulkan;
};

class FakeSampler final : public Sampler {
public:
    explicit FakeSampler(SamplerDescription description, Backend backend)
        : info_({description}), backend_(backend) {}
    Backend backend() const noexcept override { return backend_; }
    const SamplerInfo &info() const noexcept override { return info_; }

private:
    SamplerInfo info_{};
    Backend backend_ = Backend::Vulkan;
};

class FakeTarget final : public PresentationTarget {
public:
    explicit FakeTarget(PresentationTargetDescription description,
                        Backend backend = Backend::Vulkan)
        : info_({description}), backend_(backend) {}
    Backend backend() const noexcept override { return backend_; }
    const PresentationTargetInfo &info() const noexcept override {
        return info_;
    }

private:
    PresentationTargetInfo info_{};
    Backend backend_ = Backend::Vulkan;
};

class FakeDevice final : public Device {
public:
    explicit FakeDevice(Backend backend = Backend::Vulkan)
        : info_{backend, "viewport fake", "unit", 1U, 0U, 0U, 0U, 0U,
                true} {}

    struct BufferUpdate {
        Buffer *buffer = nullptr;
        std::uint64_t offset = 0U;
        std::vector<std::byte> bytes;
    };

    const DeviceInfo &info() const noexcept override { return info_; }

    BufferResult create_buffer(const BufferDescription &description,
                               std::span<const std::byte> bytes) override {
        ++buffer_calls;
        created_buffer_descriptions.push_back(description);
        if (fail_buffer_call == buffer_calls)
            return {fail_buffer_status,
                    {"fake_buffer_failed", "injected buffer failure"},
                    nullptr};
        return {BufferStatus::ready,
                {},
                std::make_unique<FakeBuffer>(description, bytes,
                                             live_buffer_count,
                                             info_.backend)};
    }

    BufferUpdateResult
    update_buffer(Buffer &buffer, std::uint64_t offset,
                  std::span<const std::byte> bytes) override {
        buffer_updates.push_back(
            {&buffer, offset, {bytes.begin(), bytes.end()}});
        return {BufferStatus::ready, {}};
    }

    TextureResult create_texture(const TextureDescription &description,
                                 const TextureUploadPlan &uploads) override {
        ++texture_calls;
        auto texture =
            std::make_unique<FakeTexture>(description, info_.backend);
        created_texture_descriptions.push_back(description);
        std::vector<std::byte> bytes;
        for (const auto &upload : uploads.subresources)
            bytes.insert(bytes.end(), upload.data.begin(), upload.data.end());
        created_texture_bytes.push_back(std::move(bytes));
        created_textures.push_back(texture.get());
        return {TextureStatus::ready, {}, std::move(texture)};
    }

    TextureUpdateResult update_texture(Texture &,
                                       const TextureUploadPlan &) override {
        return {TextureStatus::ready, {}};
    }

    TextureUpdateResult generate_texture_mips(Texture &texture) override {
        ++mip_calls;
        events.push_back("mips");
        mip_targets.push_back(&texture);
        if (fail_mip_call == mip_calls)
            return {TextureStatus::upload_failed,
                    {"fake_mip_failed", "injected mip failure"}};
        return {TextureStatus::ready, {}};
    }

    HdrLuminanceResult measure_hdr_luminance(Texture &texture) override {
        ++luminance_calls;
        events.push_back("measure_exposure");
        luminance_sources.push_back(&texture);
        if (fail_luminance)
            return {HdrLuminanceStatus::execution_failed,
                    {"fake_luminance_failed", "injected luminance failure"},
                    0.0F};
        return {HdrLuminanceStatus::ready, {}, measured_luminance};
    }

    DepthAttachmentResult create_depth_attachment(
        const DepthAttachmentDescription &description) override {
        ++depth_calls;
        created_depth_descriptions.push_back(description);
        auto depth = std::make_unique<FakeDepth>(description, info_.backend);
        created_depth_attachments.push_back(depth.get());
        return {DepthAttachmentStatus::ready, {}, std::move(depth)};
    }

    TextureClearReadbackResult
    clear_texture_and_readback(Texture &,
                               const TextureClearReadbackRequest &) override {
        return {TextureReadbackStatus::unsupported, {"unused", "unused"}, {}};
    }

    TriangleDrawResult
    draw_triangle_and_readback(Texture &,
                               const TriangleDrawRequest &) override {
        return {TriangleDrawStatus::unsupported, {"unused", "unused"}, {}};
    }

    IndexedStaticMeshBatchResult draw_indexed_static_mesh_batch_and_readback(
        Texture &texture,
        const IndexedStaticMeshBatchDescription &batch) override {
        ++draw_calls;
        if (batch.sky.has_value()) {
            events.push_back("sky");
            sky_parameters.push_back(*batch.sky);
            sky_target_subresources.push_back(batch.target_subresource);
        }
        if (batch.clouds.has_value()) {
            events.push_back("clouds");
            cloud_parameters.push_back(*batch.clouds);
            cloud_target_subresources.push_back(batch.target_subresource);
        }
        if (batch.grass.has_value()) {
            events.push_back("grass");
            grass_parameters.push_back(*batch.grass);
            grass_target_subresources.push_back(batch.target_subresource);
        }
        events.push_back("color");
        draw_targets.push_back(&texture);
        target_subresources.push_back(batch.target_subresource);
        resolve_targets.push_back(batch.resolve_target);
        capture_requests.push_back(batch.capture_rgba8);
        draw_counts.push_back(batch.draws.size());
        overlay_counts.push_back(batch.overlay_draws.size());
        selected_mesh_counts.push_back(batch.selected_mesh_draws.size());
        for (const auto &overlay : batch.overlay_draws) {
            overlay_matrices.push_back(overlay.matrices);
            overlay_buffers.push_back(overlay.vertex_buffer);
            overlay_scene_positions.push_back(overlay.scene_position);
            overlay_vertex_offsets.push_back(overlay.vertex_offset_bytes);
            overlay_vertex_counts.push_back(overlay.vertex_count);
            overlay_depth_tests.push_back(overlay.pipeline != nullptr &&
                                          overlay.pipeline->depth.test_enabled);
            overlay_depth_writes.push_back(
                overlay.pipeline != nullptr &&
                overlay.pipeline->depth.write_enabled);
        }
        std::vector<apex::scene::NodeId> nodes;
        nodes.reserve(batch.draws.size());
        for (const auto &draw : batch.draws)
            nodes.push_back(draw.packet == nullptr
                                ? apex::scene::invalid_node_id
                                : draw.packet->node);
        draw_nodes.push_back(std::move(nodes));
        if (!batch.draws.empty()) {
            draw_camera_frames.push_back(batch.draws.front().camera_frame);
            receiver_maps.push_back(
                batch.draws.front().directional_shadow_binding.maps);
            reflection_textures.push_back(
                batch.draws.front()
                    .multimap_reflection_binding.cube.texture);
        }
        for (const auto &draw : batch.draws) {
            if (draw.stock_ks_per_pixel_vulkan_source != nullptr)
                source_shadow_maps.push_back(
                    draw.stock_ks_per_pixel_vulkan_source->resources.shadow_maps);
        }
        if (fail_draw || fail_draw_call == draw_calls)
            return {IndexedStaticMeshBatchStatus::execution_failed,
                    {"fake_draw_failed", "injected draw failure"},
                    {}};
        if (invalid_draw)
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"fake_draw_invalid", "injected invalid draw"},
                    {}};
        if (unsupported_draw)
            return {IndexedStaticMeshBatchStatus::unsupported,
                    {"fake_draw_unsupported", "injected unsupported draw"},
                    {}};
        return {IndexedStaticMeshBatchStatus::ready, {}, {}};
    }

    DepthOnlyIndexedStaticMeshBatchResult
    draw_depth_only_indexed_static_mesh_batch(
        const DepthOnlyIndexedStaticMeshBatchDescription &batch) override {
        ++depth_batch_calls;
        events.push_back("shadow");
        depth_targets.push_back(batch.depth_attachment);
        depth_draw_counts.push_back(batch.draws.size());
        std::vector<apex::scene::NodeId> nodes;
        nodes.reserve(batch.draws.size());
        for (const auto &draw : batch.draws) {
            nodes.push_back(draw.packet == nullptr
                                ? apex::scene::invalid_node_id
                                : draw.packet->node);
        }
        depth_nodes.push_back(std::move(nodes));
        depth_clear_values.push_back(batch.clear_depth ? batch.depth_clear_value
                                                       : -1.0F);
        if (!batch.draws.empty() &&
            batch.draws.front().camera_frame.has_value()) {
            depth_camera_matrices.push_back(
                batch.draws.front().camera_frame->view_projection);
        }
        if (fail_depth_batch_call != 0U &&
            depth_batch_calls == fail_depth_batch_call) {
            return {DepthOnlyIndexedStaticMeshBatchStatus::execution_failed,
                    {"fake_shadow_failed", "injected shadow failure"}};
        }
        return {DepthOnlyIndexedStaticMeshBatchStatus::ready, {}};
    }

    SamplerResult
    create_sampler(const SamplerDescription &description) override {
        ++sampler_calls;
        sampler_descriptions.push_back(description);
        return {SamplerStatus::ready,
                {},
                std::make_unique<FakeSampler>(description, info_.backend)};
    }

    ShaderModuleResult
    create_shader_module(const ShaderModuleDescription &) override {
        return {ShaderModuleStatus::unsupported, {"unused", "unused"}, nullptr};
    }

    HdrToneMapResult tone_map_hdr_texture(
        Texture &source, Texture &destination,
        const HdrToneMapParameters &parameters = {}) override {
        ++tone_map_calls;
        events.push_back("tone_map");
        tone_map_sources.push_back(&source);
        tone_map_destinations.push_back(&destination);
        tone_map_parameters.push_back(parameters);
        Diagnostic diagnostic;
        const auto validation = validate_hdr_tone_map_request(
            source, destination, parameters, diagnostic);
        if (validation != HdrToneMapStatus::ready)
            return {validation, std::move(diagnostic)};
        if (tone_map_status != HdrToneMapStatus::ready)
            return {tone_map_status,
                    {"fake_tone_map_failed", "injected tone-map failure"}};
        return {HdrToneMapStatus::ready, {}};
    }

    FxaaResult apply_fxaa(Texture &source, Texture &destination) override {
        ++fxaa_calls;
        events.push_back("fxaa");
        fxaa_sources.push_back(&source);
        fxaa_destinations.push_back(&destination);
        Diagnostic diagnostic;
        const auto validation =
            validate_fxaa_request(source, destination, diagnostic);
        if (validation != FxaaStatus::ready)
            return {validation, std::move(diagnostic)};
        if (fxaa_status != FxaaStatus::ready)
            return {fxaa_status,
                    {"fake_fxaa_failed", "injected FXAA failure"}};
        return {FxaaStatus::ready, {}};
    }

    PresentationFrameResult present_texture(PresentationTarget &,
                                            Texture &texture) override {
        ++present_calls;
        events.push_back("present");
        presented_textures.push_back(&texture);
        if (fail_present)
            return {PresentationFrameStatus::execution_failed,
                    {"fake_present_failed", "injected present failure"}};
        return {PresentationFrameStatus::ready, {}};
    }

    void wait_idle() noexcept override {}

    bool fail_draw = false;
    std::size_t fail_draw_call = 0U;
    std::size_t fail_mip_call = 0U;
    bool invalid_draw = false;
    bool unsupported_draw = false;
    bool fail_present = false;
    std::size_t fail_depth_batch_call = 0U;
    std::size_t fail_buffer_call = 0U;
    BufferStatus fail_buffer_status = BufferStatus::allocation_failed;
    std::size_t buffer_calls = 0U;
    std::size_t texture_calls = 0U;
    std::size_t depth_calls = 0U;
    std::size_t sampler_calls = 0U;
    std::size_t draw_calls = 0U;
    std::size_t mip_calls = 0U;
    std::size_t luminance_calls = 0U;
    std::size_t depth_batch_calls = 0U;
    std::size_t present_calls = 0U;
    std::size_t tone_map_calls = 0U;
    std::size_t fxaa_calls = 0U;
    HdrToneMapStatus tone_map_status = HdrToneMapStatus::ready;
    FxaaStatus fxaa_status = FxaaStatus::ready;
    bool fail_luminance = false;
    float measured_luminance = 0.16F;
    std::vector<std::size_t> draw_counts;
    std::vector<std::size_t> overlay_counts;
    std::vector<std::size_t> selected_mesh_counts;
    std::vector<DrawMatrices> overlay_matrices;
    std::vector<const Buffer *> overlay_buffers;
    std::vector<std::uint32_t> overlay_scene_positions;
    std::vector<std::uint64_t> overlay_vertex_offsets;
    std::vector<std::uint32_t> overlay_vertex_counts;
    std::vector<bool> overlay_depth_tests;
    std::vector<bool> overlay_depth_writes;
    std::vector<std::vector<apex::scene::NodeId>> draw_nodes;
    std::vector<std::string> events;
    std::vector<const DepthAttachment *> depth_targets;
    std::vector<std::size_t> depth_draw_counts;
    std::vector<std::vector<apex::scene::NodeId>> depth_nodes;
    std::vector<float> depth_clear_values;
    std::vector<apex::scene::Matrix4> depth_camera_matrices;
    std::vector<BufferUpdate> buffer_updates;
    std::vector<std::array<const DepthAttachment *,
                           indexed_directional_shadow_cascade_count>>
        receiver_maps;
    std::vector<const Texture *> reflection_textures;
    std::vector<TextureDescription> created_texture_descriptions;
    std::vector<BufferDescription> created_buffer_descriptions;
    std::vector<std::vector<std::byte>> created_texture_bytes;
    std::vector<Texture *> created_textures;
    std::vector<DepthAttachmentDescription> created_depth_descriptions;
    std::vector<DepthAttachment *> created_depth_attachments;
    std::vector<SamplerDescription> sampler_descriptions;
    std::vector<std::array<const DepthAttachment *,
                           stock_ks_per_pixel_shadow_cascade_count>>
        source_shadow_maps;
    std::vector<Texture *> resolve_targets;
    std::vector<bool> capture_requests;
    std::vector<Texture *> presented_textures;
    std::vector<Texture *> tone_map_sources;
    std::vector<Texture *> tone_map_destinations;
    std::vector<HdrToneMapParameters> tone_map_parameters;
    std::vector<Texture *> fxaa_sources;
    std::vector<Texture *> fxaa_destinations;
    std::vector<Texture *> draw_targets;
    std::vector<Texture *> mip_targets;
    std::vector<Texture *> luminance_sources;
    std::vector<TextureTargetSubresource> target_subresources;
    std::vector<std::optional<CameraFrame>> draw_camera_frames;
    std::vector<PortableSkyParameters> sky_parameters;
    std::vector<TextureTargetSubresource> sky_target_subresources;
    std::vector<PortableCloudParameters> cloud_parameters;
    std::vector<TextureTargetSubresource> cloud_target_subresources;
    std::vector<PortableGrassParameters> grass_parameters;
    std::vector<TextureTargetSubresource> grass_target_subresources;
    std::shared_ptr<std::size_t> live_buffer_count =
        std::make_shared<std::size_t>(0U);

private:
    DeviceInfo info_{};
};

void put32(std::vector<std::uint8_t> &bytes, std::size_t offset,
           std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

std::vector<std::uint8_t> dxbc_shader_bytes() {
    std::vector<std::uint8_t> bytes(48U, 0U);
    put32(bytes, 0U, 0x43425844U);
    put32(bytes, 20U, 1U);
    put32(bytes, 24U, static_cast<std::uint32_t>(bytes.size()));
    put32(bytes, 28U, 1U);
    put32(bytes, 32U, 36U);
    put32(bytes, 36U, 0x58454853U);
    put32(bytes, 40U, 4U);
    return bytes;
}

std::vector<std::uint8_t> diffuse_dds() {
    std::vector<std::uint8_t> bytes(152U, 0U);
    put32(bytes, 0U, 0x20534444U);
    put32(bytes, 4U, 124U);
    put32(bytes, 12U, 1U);
    put32(bytes, 16U, 1U);
    put32(bytes, 28U, 1U);
    put32(bytes, 76U, 32U);
    put32(bytes, 80U, 4U);
    bytes[84U] = 'D';
    bytes[85U] = 'X';
    bytes[86U] = '1';
    bytes[87U] = '0';
    put32(bytes, 128U, 28U);
    put32(bytes, 132U, 3U);
    put32(bytes, 140U, 1U);
    bytes[148U] = 180U;
    bytes[149U] = 100U;
    bytes[150U] = 40U;
    bytes[151U] = 255U;
    return bytes;
}

std::vector<std::uint8_t> shader_bytes() {
    constexpr std::array<std::uint32_t, 29U> words = {
        0x07230203U, 0x00010000U, 0x00070000U, 0x00000005U, 0x00000000U,
        0x00020011U, 0x00000001U, 0x0003000eU, 0x00000000U, 0x00000001U,
        0x0005000fU, 0x00000000U, 0x00000001U, 0x6e69616dU, 0x00000000U,
        0x00020013U, 0x00000002U, 0x00030021U, 0x00000003U, 0x00000002U,
        0x00050036U, 0x00000002U, 0x00000001U, 0x00000000U, 0x00000003U,
        0x000200f8U, 0x00000004U, 0x000100fdU, 0x00010038U,
    };
    std::vector<std::uint8_t> bytes(words.size() * sizeof(std::uint32_t));
    for (std::size_t index = 0U; index < words.size(); ++index)
        for (std::size_t byte = 0U; byte < sizeof(std::uint32_t); ++byte)
            bytes[index * 4U + byte] =
                static_cast<std::uint8_t>(words[index] >> (byte * 8U));
    return bytes;
}

struct Fixture {
    apex::app::WorkspaceSessionDocument document;
    std::vector<PipelineShaderModule> modules;
    StockMaterialShaderModules module_set;
};

Fixture fixture() {
    Fixture result;
    auto& model = result.document.assembly.model;
    model.root.kind = "node";
    model.root.name = "ROOT";
    model.root.type = 1U;
    model.materials.push_back({"body", "ksPerPixel", false, false, 0U, 0U, 0U, {},
                               {{"txDiffuse", 0U, "diffuse.dds"}}, {}});
    model.textures.push_back({true, "diffuse.dds", 152U, diffuse_dds(), {}});
    apex::formats::Kn5Node mesh;
    mesh.kind = "mesh";
    mesh.name = "BODY";
    mesh.type = 2U;
    mesh.vertexStride = 11U;
    mesh.materialId = 0U;
    mesh.vertices.resize(33U, 0.0F);
    mesh.vertices[0U] = -0.5F;
    mesh.vertices[11U] = 0.5F;
    mesh.vertices[22U] = 0.0F;
    mesh.indices = {0U, 1U, 2U};
    mesh.renderable = true;
    mesh.active = true;
    mesh.visible = true;
    mesh.castShadows = true;
    apex::formats::Kn5Node hidden_model;
    hidden_model.kind = "node";
    hidden_model.name = "HIDDEN";
    hidden_model.type = 1U;
    hidden_model.active = true;
    hidden_model.visible = true;
    mesh.children.push_back(std::move(hidden_model));
    model.root.children.push_back(std::move(mesh));

    auto& scene = result.document.scene.snapshot;
    apex::scene::SceneNode root;
    root.name = "ROOT";
    const auto root_id = scene.add_node(std::move(root));
    scene.root = root_id;
    scene.workspace_kind = "generic";
    (void)scene.add_material({"body", "ksPerPixel", apex::scene::BlendMode::opaque});
    apex::scene::SceneNode body;
    body.name = "BODY";
    body.kind = apex::scene::NodeKind::mesh;
    body.material = 0U;
    body.renderable = true;
    body.visible = true;
    body.active = true;
    body.cast_shadows = true;
    body.workspace_file = "body.kn5";
    body.parent = root_id;
    const auto body_id = scene.add_node(std::move(body), root_id);
    apex::scene::SceneNode hidden;
    hidden.name = "HIDDEN";
    hidden.kind = apex::scene::NodeKind::node;
    hidden.workspace_auxiliary = "driver";
    (void)scene.add_node(std::move(hidden), body_id);
    result.document.sceneBinding.file_root_nodes = {body_id};
    result.document.assembly.workspace.kind = "generic";
    result.document.assembly.workspace.name = "viewport fixture";
    apex::workspace::WorkspaceFile workspace_file;
    workspace_file.name = "body.kn5";
    workspace_file.size = 1U;
    result.document.assembly.workspace.files.push_back(std::move(workspace_file));
    (void)body_id;

    result.modules = {
        {PipelineShaderStage::vertex, PipelineShaderFormat::spirv, shader_bytes()},
        {PipelineShaderStage::fragment, PipelineShaderFormat::spirv, shader_bytes()},
    };
    result.module_set = {StockMaterialShaderKeyKind::shader_family, "ksPerPixel",
                         result.modules};
    return result;
}

void configure_multimap_reflection(Fixture& value) {
    auto& model = value.document.assembly.model;
    auto& material = model.materials.front();
    material.shader = "ksPerPixelMultiMap";
    material.properties = {
        {"fresnelC", 0.02F, {}, {}, {}},
        {"fresnelEXP", 5.0F, {}, {}, {}},
        {"fresnelMaxLevel", 0.25F, {}, {}, {}},
        {"isAdditive", 2.0F, {}, {}, {}},
        {"nmObjectSpace", 0.0F, {}, {}, {}},
        {"useDetail", 0.0F, {}, {}, {}},
    };
    material.resources = {
        {"txDiffuse", 0U, "diffuse.dds"},
        {"txNormal", 1U, "normal.dds"},
        {"txMaps", 2U, "maps.dds"},
    };
    auto normal = model.textures.front();
    normal.name = "normal.dds";
    auto maps = model.textures.front();
    maps.name = "maps.dds";
    model.textures.push_back(std::move(normal));
    model.textures.push_back(std::move(maps));
    value.document.scene.snapshot.materials.front().shader = material.shader;
    value.module_set.key = material.shader;
    value.module_set.multimap_reflection = true;
}

Fixture car_lod_fixture() {
    auto result = fixture();
    auto& model = result.document.assembly.model;
    apex::formats::Kn5Node lod1_mesh = model.root.children.front();
    lod1_mesh.name = "BODY_LOD1";
    lod1_mesh.children.front().name = "HIDDEN_LOD1";
    model.root.children.push_back(std::move(lod1_mesh));
    apex::formats::Kn5Node auxiliary_mesh = model.root.children.front();
    auxiliary_mesh.name = "AUXILIARY";
    auxiliary_mesh.children.front().name = "HIDDEN_AUXILIARY";
    model.root.children.push_back(std::move(auxiliary_mesh));

    auto& scene = result.document.scene.snapshot;
    const auto root_id = scene.root;
    auto& lod0_scene_mesh = scene.nodes[1U];
    lod0_scene_mesh.workspace_file = "lod0.kn5";

    apex::scene::SceneNode lod1_scene_mesh;
    lod1_scene_mesh.name = "BODY_LOD1";
    lod1_scene_mesh.kind = apex::scene::NodeKind::mesh;
    lod1_scene_mesh.material = 0U;
    lod1_scene_mesh.renderable = true;
    lod1_scene_mesh.visible = true;
    lod1_scene_mesh.active = true;
    lod1_scene_mesh.cast_shadows = true;
    lod1_scene_mesh.workspace_file = "lod1.kn5";
    lod1_scene_mesh.parent = root_id;
    const auto lod1_id = scene.add_node(std::move(lod1_scene_mesh), root_id);
    apex::scene::SceneNode lod1_hidden;
    lod1_hidden.name = "HIDDEN_LOD1";
    lod1_hidden.workspace_auxiliary = "driver";
    (void)scene.add_node(std::move(lod1_hidden), lod1_id);
    apex::scene::SceneNode auxiliary_scene_mesh;
    auxiliary_scene_mesh.name = "AUXILIARY";
    auxiliary_scene_mesh.kind = apex::scene::NodeKind::mesh;
    auxiliary_scene_mesh.material = 0U;
    auxiliary_scene_mesh.renderable = true;
    auxiliary_scene_mesh.visible = true;
    auxiliary_scene_mesh.active = true;
    auxiliary_scene_mesh.cast_shadows = true;
    auxiliary_scene_mesh.workspace_file = "auxiliary.kn5";
    auxiliary_scene_mesh.parent = root_id;
    const auto auxiliary_id = scene.add_node(std::move(auxiliary_scene_mesh), root_id);
    apex::scene::SceneNode auxiliary_hidden;
    auxiliary_hidden.name = "HIDDEN_AUXILIARY";
    (void)scene.add_node(std::move(auxiliary_hidden), auxiliary_id);

    auto& workspace = result.document.assembly.workspace;
    workspace.kind = "carLods";
    workspace.files.clear();
    apex::workspace::WorkspaceFile lod0_file;
    lod0_file.name = "lod0.kn5";
    lod0_file.size = 1U;
    lod0_file.lod = apex::workspace::CarLodManifest{
        0U, "lod0.kn5", 0.0F, 15.0F, "LOD_0", 1U};
    workspace.files.push_back(std::move(lod0_file));
    apex::workspace::WorkspaceFile lod1_file;
    lod1_file.name = "lod1.kn5";
    lod1_file.size = 1U;
    lod1_file.lod = apex::workspace::CarLodManifest{
        1U, "lod1.kn5", 15.0F, 1'000'000.0F, "LOD_1", 2U};
    workspace.files.push_back(std::move(lod1_file));
    apex::workspace::WorkspaceFile auxiliary_file;
    auxiliary_file.name = "auxiliary.kn5";
    auxiliary_file.size = 1U;
    workspace.files.push_back(std::move(auxiliary_file));
    scene.workspace_kind = "carLods";
    result.document.sceneBinding.file_root_nodes = {1U, lod1_id, auxiliary_id};
    return result;
}

Fixture transparent_order_fixture() {
    auto result = fixture();
    auto& model = result.document.assembly.model;
    apex::formats::Kn5Node near_model = model.root.children.front();
    near_model.name = "GLASS_NEAR";
    near_model.children.clear();
    near_model.transparent = true;
    near_model.layer = 1U;
    apex::formats::Kn5Node far_model = near_model;
    far_model.name = "GLASS_FAR";
    model.root.children.push_back(std::move(near_model));
    model.root.children.push_back(std::move(far_model));

    auto& scene = result.document.scene.snapshot;
    scene.nodes[1U].local_aabb_center =
        apex::scene::Vector3{0.0F, 0.0F, 0.0F};
    apex::scene::SceneNode near;
    near.name = "GLASS_NEAR";
    near.kind = apex::scene::NodeKind::mesh;
    near.material = 0U;
    near.renderable = true;
    near.visible = true;
    near.active = true;
    near.transparent = true;
    near.layer = 1U;
    near.local_aabb_center = apex::scene::Vector3{0.0F, 0.0F, 0.0F};
    near.transform[14U] = 2.0F;
    const auto near_id = scene.add_node(std::move(near), scene.root);

    apex::scene::SceneNode far;
    far.name = "GLASS_FAR";
    far.kind = apex::scene::NodeKind::mesh;
    far.material = 0U;
    far.renderable = true;
    far.visible = true;
    far.active = true;
    far.transparent = true;
    far.layer = 1U;
    far.local_aabb_center = apex::scene::Vector3{0.0F, 0.0F, 0.0F};
    far.transform[14U] = 8.0F;
    const auto far_id = scene.add_node(std::move(far), scene.root);
    (void)near_id;
    (void)far_id;
    return result;
}

Fixture shadow_only_fixture() {
    auto result = fixture();
    auto& model = result.document.assembly.model;
    apex::formats::Kn5Node hidden_caster = model.root.children.front();
    hidden_caster.name = "HIDDEN_CASTER";
    hidden_caster.children.clear();
    hidden_caster.visible = false;
    hidden_caster.renderable = true;
    hidden_caster.castShadows = true;
    hidden_caster.bounds = {0.0F, 0.0F, 0.0F, 1.0F};
    model.root.children.push_back(std::move(hidden_caster));
    apex::formats::Kn5Node visible_caster = model.root.children.front();
    visible_caster.name = "VISIBLE_CASTER";
    visible_caster.children.clear();
    visible_caster.visible = true;
    visible_caster.renderable = true;
    visible_caster.castShadows = true;
    visible_caster.bounds = {0.0F, 0.0F, 0.0F, 1.0F};
    model.root.children.push_back(std::move(visible_caster));

    auto& scene = result.document.scene.snapshot;
    auto& body = scene.nodes[1U];
    body.local_bounds_source =
        apex::scene::LocalBoundsSource::kn5_serialized;
    body.local_bounds_radius = 1.0F;
    body.local_aabb_center = apex::scene::Vector3{};
    apex::scene::SceneNode caster;
    caster.name = "HIDDEN_CASTER";
    caster.kind = apex::scene::NodeKind::mesh;
    caster.material = 0U;
    caster.renderable = true;
    caster.visible = false;
    caster.active = true;
    caster.cast_shadows = true;
    caster.local_bounds_source =
        apex::scene::LocalBoundsSource::kn5_serialized;
    caster.local_bounds_radius = 1.0F;
    caster.local_aabb_center = apex::scene::Vector3{};
    (void)scene.add_node(std::move(caster), scene.root);
    apex::scene::SceneNode visible;
    visible.name = "VISIBLE_CASTER";
    visible.kind = apex::scene::NodeKind::mesh;
    visible.material = 0U;
    visible.renderable = true;
    visible.visible = true;
    visible.active = true;
    visible.cast_shadows = true;
    visible.local_bounds_source =
        apex::scene::LocalBoundsSource::kn5_serialized;
    visible.local_bounds_radius = 1.0F;
    visible.local_aabb_center = apex::scene::Vector3{};
    (void)scene.add_node(std::move(visible), scene.root);
    return result;
}

WorkspaceViewportPrepareRequest request_for(const Fixture& fixture_value) {
    WorkspaceViewportPrepareRequest request;
    request.presentation.width = 32U;
    request.presentation.height = 32U;
    request.presentation.format = TextureFormat::rgba8_unorm;
    request.shader_modules = std::span<const StockMaterialShaderModules>(
        &fixture_value.module_set, 1U);
    request.render.camera_position = {0.0F, 0.0F, 5.0F};
    request.render.include_reflections = false;
    request.render.include_shadows = false;
    return request;
}

DecodedTexturePlan portable_cloud_texture_plan() {
    DecodedTexturePlan plan;
    plan.description.width = 1U;
    plan.description.height = 1U;
    plan.description.mip_levels = 1U;
    plan.description.array_layers = 1U;
    plan.description.format = TextureFormat::rgba8_unorm;
    plan.description.usage = TextureUsage::sampled;
    plan.description.memory = TextureMemory::device_local;
    plan.description.mutability = TextureMutability::immutable;
    plan.levels.push_back({1U, 1U, {std::uint8_t{255U}, std::uint8_t{128U},
                                    std::uint8_t{64U}, std::uint8_t{255U}}});
    return plan;
}

WorkspaceViewportPortableCloudOptions portable_cloud_options(
    const DecodedTexturePlan& texture) {
    WorkspaceViewportPortableCloudOptions options;
    options.settings.count = 100U;
    options.build.texture_count = 7U;
    options.cloud_cover = 1.0F;
    options.cloud_cutoff = 0.7F;
    options.cloud_color = 1.0F;
    options.textures[0U] = &texture;
    return options;
}

PortableGrassSourceTriangle portable_grass_triangle() {
    PortableGrassSourceTriangle triangle;
    triangle.vertices[0U].position = {0.0F, 0.0F, 0.0F};
    triangle.vertices[1U].position = {0.0F, 0.0F, 2.0F};
    triangle.vertices[2U].position = {2.0F, 0.0F, 0.0F};
    triangle.source_id = 17U;
    return triangle;
}

WorkspaceViewportPortableGrassOptions portable_grass_options(
    const std::array<PortableGrassSourceTriangle, 1U>& triangles,
    const DecodedTexturePlan& atlas) {
    WorkspaceViewportPortableGrassOptions options;
    options.triangles = triangles;
    options.settings.density = 1.0F;
    options.settings.atlas_columns = 1U;
    options.settings.atlas_rows = 1U;
    options.build.max_blades = 4U;
    options.build.max_candidates = 4U;
    options.build.declared_triangle_count = triangles.size();
    options.build.seed = 29U;
    options.atlas = &atlas;
    options.frame.wetness = 0.25F;
    options.frame.wind_direction = {1.0F, 0.0F};
    options.frame.wind_strength = 0.5F;
    options.frame.elapsed_seconds = 1.5F;
    return options;
}

apex::formats::AcdArchive texture_archive(std::vector<std::uint8_t> bytes) {
    apex::formats::AcdArchive archive;
    archive.source = "fixture/data.acd";
    archive.assetName = "fixture";
    apex::formats::AcdEntry entry;
    entry.name = "override.dds";
    entry.path = "override.dds";
    entry.safe = true;
    entry.size = bytes.size();
    entry.data = std::move(bytes);
    archive.entries.push_back(std::move(entry));
    return archive;
}

void materializes_external_texture_before_backend_preparation() {
    for (const auto backend : {Backend::Vulkan, Backend::D3D12}) {
        auto value = fixture();
        if (backend == Backend::D3D12) {
            for (auto& module : value.modules) {
                module.format = PipelineShaderFormat::dxbc;
                module.bytes = dxbc_shader_bytes();
            }
        }

        auto replacement = diffuse_dds();
        replacement[148U] = 7U;
        replacement[149U] = 19U;
        replacement[150U] = 31U;
        apex::assets::AssetSource source;
        source.addAcdArchive(texture_archive(replacement));

        std::vector<MaterialBindingOverrides> overrides(1U);
        MaterialTextureOverride override_value;
        override_value.file = "override.dds";
        overrides.front().resources.emplace("txDiffuse", override_value);

        const std::array<ExternalTextureGrant, 1U> grants = {
            ExternalTextureGrant{"viewport", &source}};
        std::array<ExternalTextureRequest, 1U> external_requests{};
        auto& external = external_requests.front();
        external.grant_id = "viewport";
        external.material_index = 0U;
        external.binding.slot = "txDiffuse";
        external.binding.kind = MaterialTextureKind::external_file;
        external.binding.bind_point = 0U;
        external.binding.file = "override.dds";
        external.binding.required = true;

        auto request = request_for(value);
        request.overrides_by_material = overrides;
        request.external_textures =
            apex::app::WorkspaceViewportExternalTextureRequest{
                grants, external_requests, {}};
        FakeDevice device(backend);
        auto prepared = apex::app::prepareWorkspaceViewport(
            device, value.document, request);

        const std::vector<std::byte> expected = {
            std::byte{7U}, std::byte{19U}, std::byte{31U}, std::byte{255U}};
        require(prepared.ok() && std::find(device.created_texture_bytes.begin(), device.created_texture_bytes.end(),
                                           expected) != device.created_texture_bytes.end(),
                "external DDS is copied and uploaded through each backend "
                "contract");
        require(overrides.front().resources.at("txDiffuse").file == "override.dds",
                "external texture preparation does not mutate caller overrides");
    }
}

void materializes_solid_color_before_backend_preparation() {
    for (const auto backend : {Backend::Vulkan, Backend::D3D12}) {
        auto value = fixture();
        if (backend == Backend::D3D12) {
            for (auto& module : value.modules) {
                module.format = PipelineShaderFormat::dxbc;
                module.bytes = dxbc_shader_bytes();
            }
        }

        std::vector<MaterialBindingOverrides> overrides(1U);
        MaterialTextureOverride override_value;
        override_value.color = std::array<float, 4>{0.125F, 0.5F, 0.75F, 1.0F};
        overrides.front().resources.emplace("txDiffuse", override_value);

        auto request = request_for(value);
        request.overrides_by_material = overrides;
        FakeDevice device(backend);
        auto prepared = apex::app::prepareWorkspaceViewport(device, value.document, request);

        const std::vector<std::byte> expected = {std::byte{32U}, std::byte{128U}, std::byte{191U}, std::byte{255U}};
        require(prepared.ok() && std::find(device.created_texture_bytes.begin(), device.created_texture_bytes.end(),
                                           expected) != device.created_texture_bytes.end(),
                "solid color is decoded and uploaded through each backend contract");
        require(overrides.front().resources.at("txDiffuse").color == override_value.color,
                "solid-color preparation does not mutate caller overrides");
    }
}

void rejects_invalid_solid_color_before_gpu_allocation() {
    auto value = fixture();
    std::vector<MaterialBindingOverrides> overrides(1U);
    MaterialTextureOverride override_value;
    override_value.color = std::array<float, 4>{std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 1.0F};
    overrides.front().resources.emplace("txDiffuse", override_value);

    auto request = request_for(value);
    request.overrides_by_material = overrides;
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(!prepared.ok() && prepared.status == apex::app::WorkspaceViewportStatus::invalid &&
                prepared.diagnostic.code == "solid_color_non_finite" && device.texture_calls == 0U &&
                device.depth_calls == 0U && device.buffer_calls == 0U,
            "non-finite solid color fails before GPU allocation");

    overrides.front().resources.at("txDiffuse").color =
        std::array<float, 4>{0.25F, 0.5F, 0.75F, 1.0F};
    MaterialTextureOverride unresolved;
    unresolved.file = "not-authorized.dds";
    overrides.front().resources.emplace("txNormal", unresolved);
    request.overrides_by_material = overrides;
    prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(!prepared.ok() &&
                prepared.status ==
                    apex::app::WorkspaceViewportStatus::unsupported &&
                prepared.diagnostic.code ==
                    "workspace_viewport_texture_override_unresolved" &&
                device.texture_calls == 0U && device.depth_calls == 0U &&
                device.buffer_calls == 0U,
            "unresolved resource override fails before GPU allocation");
}

void rejects_invalid_external_textures_before_gpu_allocation() {
    auto value = fixture();
    std::vector<MaterialBindingOverrides> overrides(1U);
    MaterialTextureOverride override_value;
    override_value.file = "override.dds";
    overrides.front().resources.emplace("txDiffuse", override_value);

    apex::assets::AssetSource source;
    source.addAcdArchive(texture_archive({1U, 2U, 3U}));
    const std::array<ExternalTextureGrant, 1U> grants = {ExternalTextureGrant{"viewport", &source}};
    std::array<ExternalTextureRequest, 1U> external_requests{};
    auto& external = external_requests.front();
    external.grant_id = "viewport";
    external.material_index = 0U;
    external.binding.slot = "txDiffuse";
    external.binding.kind = MaterialTextureKind::external_file;
    external.binding.bind_point = 0U;
    external.binding.file = "override.dds";

    auto request = request_for(value);
    request.overrides_by_material = overrides;
    request.external_textures = apex::app::WorkspaceViewportExternalTextureRequest{grants, external_requests, {}};
    FakeDevice device;
    auto malformed = apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(!malformed.ok() && malformed.status == apex::app::WorkspaceViewportStatus::invalid &&
                malformed.diagnostic.code == "external_texture_decode_invalid_header" && device.texture_calls == 0U &&
                device.depth_calls == 0U && device.buffer_calls == 0U,
            "truncated external DDS fails before GPU allocation");

    apex::assets::AssetSource scoped_source;
    scoped_source.addAcdArchive(texture_archive(diffuse_dds()));
    const std::array<ExternalTextureGrant, 1U> scoped_grants = {ExternalTextureGrant{"viewport", &scoped_source}};
    external.workspace_file_index = 1U;
    request.external_textures =
        apex::app::WorkspaceViewportExternalTextureRequest{scoped_grants, external_requests, {}};
    auto wrong_scope = apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(!wrong_scope.ok() && wrong_scope.diagnostic.code == "external_texture_workspace_scope_mismatch" &&
                device.texture_calls == 0U && device.depth_calls == 0U && device.buffer_calls == 0U,
            "external texture scope mismatch fails before GPU allocation");

    request.external_textures = apex::app::WorkspaceViewportExternalTextureRequest{grants, {}, {}};
    auto empty = apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(!empty.ok() && empty.diagnostic.code == "workspace_viewport_external_texture_request_empty" &&
                device.texture_calls == 0U && device.depth_calls == 0U && device.buffer_calls == 0U,
            "empty external texture handoff fails before GPU allocation");
}

PipelineProgram opaque_shadow_pipeline(const Fixture& fixture_value) {
    PipelineProgram pipeline;
    pipeline.name = "viewport-opaque-directional-shadow";
    pipeline.shaders = {fixture_value.modules.front()};
    pipeline.vertex_layout.stride = 11U * sizeof(float);
    pipeline.vertex_layout.attributes = {
        {PipelineVertexSemantic::position,
         PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {PipelineVertexSemantic::normal,
         PipelineVertexAttributeFormat::float32x3, 1U, 12U},
        {PipelineVertexSemantic::texcoord0,
         PipelineVertexAttributeFormat::float32x2, 2U, 24U},
        {PipelineVertexSemantic::tangent,
         PipelineVertexAttributeFormat::float32x3, 3U, 32U},
    };
    pipeline.targets.has_depth = true;
    pipeline.targets.depth = {
        PipelineRenderTargetFormat::depth32_float, 1U};
    pipeline.depth.test_enabled = true;
    pipeline.depth.write_enabled = true;
    pipeline.depth.compare = PipelineCompareOperation::less;
    pipeline.transform_contract = PipelineTransformContract::draw_matrices;
    return pipeline;
}

PipelineProgram alpha_shadow_pipeline(const Fixture& fixture_value) {
    auto built = apex::app::buildWorkspaceShadowPipeline(
        "viewport-alpha-directional-shadow",
        {fixture_value.modules[0], fixture_value.modules[1]},
        DepthOnlyIndexedPipelineRole::stock_alpha_tested_static);
    if (!built.ok())
        throw std::runtime_error("alpha shadow pipeline fixture failed");
    return std::move(*built.pipeline);
}

PipelineProgram skinned_shadow_pipeline(const Fixture& fixture_value) {
    auto built = apex::app::buildWorkspaceShadowPipeline(
        "viewport-skinned-directional-shadow",
        {fixture_value.modules[0]},
        DepthOnlyIndexedPipelineRole::skinned);
    if (!built.ok())
        throw std::runtime_error("skinned shadow pipeline fixture failed");
    return std::move(*built.pipeline);
}

PipelineProgram authoring_overlay_pipeline(const Fixture& fixture_value,
                                           std::uint32_t samples = 1U) {
    PipelineProgram pipeline;
    pipeline.name = "viewport-authoring-overlay";
    pipeline.shaders = fixture_value.modules;
    pipeline.vertex_layout.stride = sizeof(OverlayLineVertex);
    pipeline.vertex_layout.attributes = {
        {PipelineVertexSemantic::position,
         PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {PipelineVertexSemantic::color,
         PipelineVertexAttributeFormat::float32x3, 1U, 12U},
    };
    pipeline.targets.colors = {
        {PipelineRenderTargetFormat::rgba8_unorm, samples}};
    pipeline.targets.has_depth = true;
    pipeline.targets.depth = {
        PipelineRenderTargetFormat::depth32_float, samples};
    pipeline.raster.cull = PipelineCullMode::none;
    pipeline.raster.fill = PipelineFillMode::wireframe;
    pipeline.depth.test_enabled = false;
    pipeline.depth.write_enabled = false;
    pipeline.transform_contract = PipelineTransformContract::draw_matrices;
    return pipeline;
}

PipelineProgram ai_spline_pipeline(const Fixture& fixture_value,
                                   std::uint32_t samples = 1U) {
    PipelineProgram pipeline =
        authoring_overlay_pipeline(fixture_value, samples);
    pipeline.name = "viewport-ai-spline-raw";
    pipeline.depth.test_enabled = true;
    pipeline.depth.write_enabled = true;
    pipeline.depth.compare = PipelineCompareOperation::less_or_equal;
    return pipeline;
}

PipelineProgram ai_spline_interval_pipeline(const Fixture& fixture_value,
                                            std::uint32_t samples = 1U) {
    PipelineProgram pipeline = ai_spline_pipeline(fixture_value, samples);
    pipeline.name = "viewport-ai-spline-interval";
    pipeline.depth.test_enabled = false;
    pipeline.depth.write_enabled = false;
    return pipeline;
}

PipelineProgram ai_spline_side_pipeline(const Fixture& fixture_value,
                                         std::uint32_t samples = 1U) {
    PipelineProgram pipeline = ai_spline_pipeline(fixture_value, samples);
    pipeline.name = "viewport-ai-spline-side";
    return pipeline;
}

PipelineProgram ai_spline_camber_pipeline(const Fixture& fixture_value,
                                           std::uint32_t samples = 1U) {
    PipelineProgram pipeline = ai_spline_pipeline(fixture_value, samples);
    pipeline.name = "viewport-ai-spline-camber";
    return pipeline;
}

PipelineProgram selected_mesh_pipeline(const Fixture& fixture_value,
                                       std::uint32_t samples = 1U) {
    PipelineProgram pipeline;
    pipeline.name = "viewport-selected-mesh";
    pipeline.shaders = fixture_value.modules;
    pipeline.vertex_layout.stride = 11U * sizeof(float);
    pipeline.vertex_layout.attributes = {
        {PipelineVertexSemantic::position,
         PipelineVertexAttributeFormat::float32x3, 0U, 0U},
    };
    pipeline.targets.colors = {
        {PipelineRenderTargetFormat::rgba8_unorm, samples}};
    pipeline.targets.has_depth = true;
    pipeline.targets.depth = {
        PipelineRenderTargetFormat::depth32_float, samples};
    pipeline.raster.fill = PipelineFillMode::solid;
    pipeline.raster.cull = PipelineCullMode::front;
    pipeline.depth.test_enabled = false;
    pipeline.depth.write_enabled = false;
    pipeline.transform_contract = PipelineTransformContract::selected_mesh;
    return pipeline;
}

CameraFrame valid_shadow_camera(
    float x = 0.0F,
    CameraClipSpace clip_space = CameraClipSpace::vulkan) {
    CameraFrameRequest request;
    request.eye = {x, 0.0F, 5.0F};
    request.target = {0.0F, 0.0F, 0.0F};
    request.aspect = 1.0F;
    request.near_plane = 0.01F;
    request.far_plane = 100.0F;
    request.clip_space = clip_space;
    const auto built = build_camera_frame(request);
    require(built.ok(), "directional shadow test camera builds");
    return *built.frame;
}

WorkspaceViewportStockVulkanSourceFrame valid_source_frame(
    const CameraFrame& camera = CameraFrame{}) {
    WorkspaceViewportStockVulkanSourceFrame frame;
    frame.camera = make_stock_ks_per_pixel_camera_constants(
        camera.view, camera.projection,
        apex::scene::identity_matrix, {0.0F, 0.0F, 2.0F}, 0.1F, 100.0F,
        camera.fov_radians * 57.2957795130823208768F);
    frame.camera.camera_position = camera.position;
    frame.camera.near_plane = camera.near_plane;
    frame.camera.far_plane = camera.far_plane;
    frame.lighting.light_direction = {0.0F, 0.0F, -1.0F};
    frame.lighting.ambient_color = {0.1F, 0.1F, 0.1F, 1.0F};
    frame.lighting.light_color = {0.8F, 0.8F, 0.8F};
    return frame;
}

void builds_checked_stock_vulkan_source_frames() {
    const auto evaluated = apex::app::evaluateWorkspaceViewportLighting({});
    require(evaluated.ok(), "stock weather evaluates for native b2 coverage");
    const auto native_lighting =
        apex::app::buildWorkspaceViewportStockVulkanSourceLighting(
            evaluated.evaluated, 1920U, 1080U);
    require(native_lighting.has_value() &&
                native_lighting->light_direction ==
                    std::array<float, 3U>{
                        -evaluated.evaluated.sun_direction[0],
                        -evaluated.evaluated.sun_direction[1],
                        -evaluated.evaluated.sun_direction[2]} &&
                native_lighting->ambient_color[3U] == 1.0F &&
                native_lighting->screen_width == 1.0F / 1920.0F &&
                native_lighting->screen_height == 1.0F / 1080.0F &&
                native_lighting->exposure == 2.0F &&
                native_lighting->minimum_exposure == 0.0F &&
                native_lighting->maximum_exposure == 10000.0F &&
                native_lighting->dof_focus == 400.0F &&
                native_lighting->dof_range == 500.0F &&
                native_lighting->saturation == 1.0F &&
                native_lighting->cloud_offset == 0.0F &&
                native_lighting->game_time == 0.0F &&
                native_lighting->unknown_152 ==
                    std::array<float, 2U>{0.0F, 0.0F},
            "native b2 builder maps weather, reciprocals, and recovered startup values");
    require(!apex::app::buildWorkspaceViewportStockVulkanSourceLighting(
                 evaluated.evaluated, 0U, 1080U),
            "native b2 builder rejects zero-sized viewports");
    auto invalid_evaluated = evaluated.evaluated;
    invalid_evaluated.fog_distance =
        std::numeric_limits<float>::quiet_NaN();
    require(!apex::app::buildWorkspaceViewportStockVulkanSourceLighting(
                 invalid_evaluated, 1920U, 1080U),
            "native b2 builder rejects non-finite evaluated lighting");

    const CameraFrame camera = valid_shadow_camera(1.25F);
    StockKsPerPixelLightingConstants lighting;
    lighting.light_direction = {0.1F, -0.9F, 0.2F};
    lighting.ambient_color = {0.2F, 0.3F, 0.4F, 1.0F};
    lighting.light_color = {0.9F, 0.8F, 0.7F};
    lighting.fog_linear = 1200.0F;
    lighting.fog_blend = 0.6F;
    const auto built =
        apex::app::buildWorkspaceViewportStockVulkanSourceFrame(
            camera, lighting);
    const auto inverse = invert_camera_matrix(camera.view_projection);
    require(built.has_value() && inverse.has_value() &&
                built->camera.view ==
                    stock_ks_per_pixel_transpose_matrix(camera.view) &&
                built->camera.projection ==
                    stock_ks_per_pixel_transpose_matrix(camera.projection) &&
                built->camera.mvp_inverse ==
                    stock_ks_per_pixel_transpose_matrix(*inverse) &&
                built->camera.camera_position == camera.position &&
                built->camera.near_plane == camera.near_plane &&
                built->camera.far_plane == camera.far_plane &&
                built->camera.field_of_view ==
                    camera.fov_radians * 57.2957795130823208768F &&
                built->lighting.fog_linear == 1200.0F &&
                built->lighting.fog_blend == 0.6F,
            "source frame builder maps checked Vulkan camera and authored b2");

    CameraFrame wrong_backend = camera;
    wrong_backend.clip_space = CameraClipSpace::d3d12;
    require(!apex::app::buildWorkspaceViewportStockVulkanSourceFrame(
                 wrong_backend, lighting),
            "source frame builder rejects non-Vulkan camera state");

    CameraFrame singular = camera;
    singular.view_projection = {};
    require(!apex::app::buildWorkspaceViewportStockVulkanSourceFrame(
                 singular, lighting),
            "source frame builder rejects singular camera state");

    const CameraFrame d3d12_camera =
        valid_shadow_camera(1.25F, CameraClipSpace::d3d12);
    const auto d3d12_built =
        apex::app::buildWorkspaceViewportStockD3D12NativeFrame(
            d3d12_camera, lighting);
    require(d3d12_built.has_value() &&
                d3d12_built->camera.view ==
                    stock_ks_per_pixel_transpose_matrix(d3d12_camera.view) &&
                d3d12_built->camera.projection ==
                    stock_ks_per_pixel_transpose_matrix(
                        d3d12_camera.projection) &&
                d3d12_built->camera.camera_position ==
                    d3d12_camera.position,
            "native frame builder accepts the D3D12 camera convention");
    require(!apex::app::buildWorkspaceViewportStockD3D12NativeFrame(
                 camera, lighting) &&
                !apex::app::buildWorkspaceViewportStockVulkanSourceFrame(
                    d3d12_camera, lighting),
            "stock native frame builders reject cross-backend cameras");

    lighting.game_time = std::numeric_limits<float>::quiet_NaN();
    require(!apex::app::buildWorkspaceViewportStockVulkanSourceFrame(
                 camera, lighting),
            "source frame builder rejects non-finite native lighting");
}

void evaluates_bounded_workspace_lighting() {
    const auto default_lighting =
        apex::app::evaluateWorkspaceViewportLighting({});
    require(default_lighting.ok() &&
                default_lighting.evaluated.preset.id == "5_light_clouds",
            "workspace lighting selects the production default weather");
    const auto& direction = default_lighting.evaluated.sun_direction;
    require(std::abs(direction[0] - 0.3686878F) < 1.0e-5F &&
                std::abs(direction[1] - 0.8191520F) < 1.0e-5F &&
                std::abs(direction[2] - 0.4393850F) < 1.0e-5F &&
                default_lighting.frame_constants.sun_direction ==
                    std::array<float, 4U>{direction[0], direction[1],
                                          direction[2], 0.0F} &&
                default_lighting.frame_constants.sun_color ==
                    std::array<float, 4U>{
                        default_lighting.evaluated.sun_color[0],
                        default_lighting.evaluated.sun_color[1],
                        default_lighting.evaluated.sun_color[2], 0.0F} &&
                default_lighting.frame_constants.ambient_color ==
                    std::array<float, 4U>{
                        default_lighting.evaluated.ambient_color[0],
                        default_lighting.evaluated.ambient_color[1],
                        default_lighting.evaluated.ambient_color[2], 0.0F} &&
                default_lighting.frame_constants.horizon_color ==
                    std::array<float, 4U>{
                        default_lighting.evaluated.horizon_color[0],
                        default_lighting.evaluated.horizon_color[1],
                        default_lighting.evaluated.horizon_color[2], 0.0F} &&
                default_lighting.frame_constants.sky_color ==
                    std::array<float, 4U>{
                        default_lighting.evaluated.sky_color[0],
                        default_lighting.evaluated.sky_color[1],
                        default_lighting.evaluated.sky_color[2], 0.0F} &&
                default_lighting.frame_constants.fog_color ==
                    std::array<float, 4U>{
                        default_lighting.evaluated.fog_color[0],
                        default_lighting.evaluated.fog_color[1],
                        default_lighting.evaluated.fog_color[2], 0.0F} &&
                default_lighting.frame_constants.fog ==
                    std::array<float, 4U>{
                        default_lighting.evaluated.fog_distance,
                        default_lighting.evaluated.fog_blend, 1.0F, 0.0F} &&
                default_lighting.frame_constants.camera_position ==
                    std::array<float, 4U>{},
            "workspace lighting packs the source-matched atmosphere frame ABI");

    require(apex::app::evaluateWorkspaceViewportLighting(
                {"3_clear", 120.0F, 35.0F}).ok(),
            "workspace lighting accepts a bounded stock weather selection");
    require(!apex::app::evaluateWorkspaceViewportLighting(
                 {"missing", 40.0F, 55.0F}).ok() &&
                !apex::app::evaluateWorkspaceViewportLighting(
                 {"", std::numeric_limits<float>::quiet_NaN(), 55.0F}).ok() &&
                !apex::app::evaluateWorkspaceViewportLighting(
                 {"", 40.0F, 91.0F}).ok(),
            "workspace lighting rejects unknown and malformed state");
}

void camera_controller_matches_bounded_editor_gestures() {
    apex::app::WorkspaceViewportCameraController controller;
    const auto initial = controller.frame(1.0F, CameraClipSpace::vulkan);
    require(initial.ok(), "default viewport camera builds for Vulkan");
    require(std::abs(initial.frame->position[0] - 3.025F) < 0.01F &&
                std::abs(initial.frame->position[1] - 1.714F) < 0.01F &&
                std::abs(initial.frame->position[2] - 3.593F) < 0.01F,
            "default viewport camera retains the browser orbit state");

    require(controller.apply({
                apex::app::WorkspaceViewportCameraGesture::begin_orbit,
                0.0F, 0.0F}),
            "orbit gesture starts");
    require(controller.apply({
                apex::app::WorkspaceViewportCameraGesture::drag, 10.0F, -5.0F}),
            "orbit drag updates camera state");
    const auto orbited = controller.frame(2.0F, CameraClipSpace::d3d12);
    require(orbited.ok() && orbited.frame->clip_space == CameraClipSpace::d3d12 &&
                orbited.frame->position != initial.frame->position,
            "orbit drag produces a backend-neutral camera frame");
    require(controller.apply({
                apex::app::WorkspaceViewportCameraGesture::end_drag,
                0.0F, 0.0F}),
            "orbit gesture ends");
    const auto stopped = controller.frame(2.0F, CameraClipSpace::d3d12);
    require(!controller.apply({
                apex::app::WorkspaceViewportCameraGesture::drag, 10.0F, 10.0F}) &&
                stopped.frame->position == controller.frame(
                    2.0F, CameraClipSpace::d3d12).frame->position,
            "drag after release does not mutate camera state");

    require(controller.apply({
                apex::app::WorkspaceViewportCameraGesture::begin_pan,
                0.0F, 0.0F}),
            "pan gesture starts");
    const auto before_pan = controller.target;
    require(controller.apply({
                apex::app::WorkspaceViewportCameraGesture::drag, 20.0F, -10.0F}) &&
                controller.target != before_pan,
            "pan drag updates the orbit target");
    (void)controller.apply({apex::app::WorkspaceViewportCameraGesture::end_drag,
                            0.0F, 0.0F});

    const float before_zoom = controller.distance;
    require(controller.apply({apex::app::WorkspaceViewportCameraGesture::wheel,
                              0.0F, 20.0F}) && controller.distance > before_zoom,
            "wheel gesture zooms out");
    require(controller.apply({apex::app::WorkspaceViewportCameraGesture::wheel,
                              0.0F, 100'000.0F}) &&
                controller.distance == 1.0e7F,
            "wheel gesture is clamped to its bounded distance");
    const float bounded_distance = controller.distance;
    require(!controller.apply({
                apex::app::WorkspaceViewportCameraGesture::wheel, 0.0F,
                std::numeric_limits<float>::quiet_NaN()}) &&
                controller.distance == bounded_distance,
            "non-finite wheel input is rejected atomically");
}

void camera_controller_supports_keyboard_translation() {
    apex::app::WorkspaceViewportCameraController controller;
    const auto before = controller.frame(1.0F, CameraClipSpace::vulkan);
    require(before.ok(), "keyboard camera starts from a valid frame");
    const auto before_target = controller.target;
    require(controller.move(apex::app::WorkspaceViewportCameraMove::forward) &&
                controller.target != before_target,
            "forward movement translates the orbit target");
    const auto after_forward = controller.frame(1.0F, CameraClipSpace::vulkan);
    require(after_forward.ok() && after_forward.frame->position != before.frame->position,
            "forward movement changes the camera frame");

    const auto moved_target = controller.target;
    require(controller.move(apex::app::WorkspaceViewportCameraMove::backward) &&
                std::abs(controller.target[0] - moved_target[0]) > 0.0F,
            "backward movement is the inverse translation");
    require(controller.move(apex::app::WorkspaceViewportCameraMove::left) &&
                controller.move(apex::app::WorkspaceViewportCameraMove::right),
            "horizontal movement remains finite");
    require(controller.move(apex::app::WorkspaceViewportCameraMove::up) &&
                controller.move(apex::app::WorkspaceViewportCameraMove::down),
            "world-up movement remains finite");

    const auto stable_target = controller.target;
    require(!controller.move(apex::app::WorkspaceViewportCameraMove::forward,
                             std::numeric_limits<float>::quiet_NaN()) &&
                controller.target == stable_target,
            "non-finite keyboard distance is rejected atomically");
}

void opens_and_draws() {
    auto value = fixture();
    auto request = request_for(value);
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "viewport preparation succeeds with embedded KN5 texture");
    require(prepared.viewport->renderPlan().items.size() == 1U,
            "viewport preparation retains the scene render plan");
    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    const std::array<std::uint8_t, 1U> visible = {1U};
    frame.packet_visibility = visible;
    Diagnostic diagnostic;
    const auto status = prepared.viewport->drawAndPresent(device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::ready &&
                device.draw_calls == 1U && device.present_calls == 1U &&
                device.draw_counts == std::vector<std::size_t>({1U}),
            "viewport draws before presenting one frame");

    const std::array<std::uint8_t, 1U> hidden = {0U};
    frame.packet_visibility = hidden;
    const auto clear_only_status =
        prepared.viewport->drawAndPresent(device, target, frame, diagnostic);
    require(clear_only_status == WorkspaceViewportFrameStatus::ready &&
                device.draw_calls == 2U && device.present_calls == 2U &&
                device.draw_counts == std::vector<std::size_t>({1U, 0U}),
            "all-hidden viewport frame clears and presents without rebuilding resources");
}

void draws_portable_sky_before_main_color_and_requires_constants() {
    auto value = fixture();
    auto request = request_for(value);
    request.sky_enabled = true;
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "portable sky viewport preparation succeeds");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera = valid_shadow_camera();
    const auto lighting = apex::app::evaluateWorkspaceViewportLighting({});
    require(lighting.ok(), "portable sky test lighting evaluates");
    frame.frame_constants = lighting.frame_constants;
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(device, target, frame,
                                              diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.draw_calls == 1U && device.present_calls == 1U &&
                device.events == std::vector<std::string>(
                    {"sky", "color", "present"}) &&
                device.sky_parameters.size() == 1U &&
                device.sky_target_subresources.size() == 1U,
            "portable sky is one retained submission before main color and present");
    const auto& sky = device.sky_parameters.front();
    require(sky.camera.view_projection == frame.camera.view_projection,
            "main portable sky uses the current camera");
    for (std::size_t component = 0U; component < 3U; ++component) {
        require(std::abs(sky.horizon_color[component] -
                         lighting.frame_constants.horizon_color[component]) <
                    1.0e-6F &&
                    std::abs(sky.sky_color[component] -
                             lighting.frame_constants.sky_color[component]) <
                        1.0e-6F &&
                    std::abs(sky.sun_color[component] -
                             lighting.frame_constants.sun_color[component]) <
                        1.0e-6F &&
                    std::abs(sky.sun_direction[component] -
                             lighting.frame_constants.sun_direction[component]) <
                        1.0e-6F,
                "portable sky forwards evaluated frame lighting constants");
    }

    auto missing_constants_request = request_for(value);
    missing_constants_request.sky_enabled = true;
    FakeDevice missing_constants_device;
    auto missing_constants_prepared = apex::app::prepareWorkspaceViewport(
        missing_constants_device, value.document, missing_constants_request);
    require(missing_constants_prepared.ok(),
            "missing sky constants viewport preparation succeeds");
    FakeTarget missing_constants_target(missing_constants_request.presentation);
    WorkspaceViewportFrameRequest missing_constants_frame;
    missing_constants_frame.camera = valid_shadow_camera();
    require(missing_constants_prepared.viewport->drawAndPresent(
                missing_constants_device, missing_constants_target,
                missing_constants_frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "workspace_viewport_sky_constants_missing" &&
                missing_constants_device.draw_calls == 0U &&
                missing_constants_device.present_calls == 0U,
            "enabled portable sky rejects a frame without lighting constants");

    auto disabled_request = request_for(value);
    FakeDevice disabled_device;
    auto disabled_prepared = apex::app::prepareWorkspaceViewport(
        disabled_device, value.document, disabled_request);
    require(disabled_prepared.ok(), "disabled portable sky preparation succeeds");
    FakeTarget disabled_target(disabled_request.presentation);
    WorkspaceViewportFrameRequest disabled_frame;
    disabled_frame.camera = valid_shadow_camera();
    disabled_frame.frame_constants = lighting.frame_constants;
    require(disabled_prepared.viewport->drawAndPresent(
                disabled_device, disabled_target, disabled_frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                disabled_device.events == std::vector<std::string>(
                    {"color", "present"}) &&
                disabled_device.sky_parameters.empty(),
            "disabled portable sky preserves the color-only path");
}

void prepares_and_draws_portable_clouds_with_retained_resources() {
    for (const auto backend : {Backend::Vulkan, Backend::D3D12}) {
        auto value = fixture();
        if (backend == Backend::D3D12) {
            for (auto& module : value.modules) {
                module.format = PipelineShaderFormat::dxbc;
                module.bytes = dxbc_shader_bytes();
            }
        }
        const auto texture = portable_cloud_texture_plan();
        auto request = request_for(value);
        request.portable_clouds = portable_cloud_options(texture);
        FakeDevice device(backend);
        auto prepared = apex::app::prepareWorkspaceViewport(
            device, value.document, request);
        if (!prepared.ok())
            throw std::runtime_error("portable cloud preparation failed: " +
                                     prepared.diagnostic.code + ": " +
                                     prepared.diagnostic.message);
        const auto cloud_layout = buildPortableCloudLayout(
            request.portable_clouds->settings, request.portable_clouds->build);
        const auto cloud_buffer = std::find_if(
            device.created_buffer_descriptions.begin(),
            device.created_buffer_descriptions.end(), [&](const auto& description) {
                return description.size_bytes ==
                           cloud_layout.vertices.size() *
                               portable_cloud_vertex_stride_bytes &&
                       description.usage == BufferUsage::vertex &&
                       description.memory == BufferMemory::device_local &&
                       description.mutability == BufferMutability::immutable;
            });
        require(cloud_buffer != device.created_buffer_descriptions.end() &&
                    device.texture_calls >= 1U &&
                    !device.sampler_descriptions.empty() &&
                    device.sampler_descriptions.back().min_filter ==
                        SamplerFilter::linear &&
                    device.sampler_descriptions.back().mag_filter ==
                        SamplerFilter::linear &&
                    device.sampler_descriptions.back().mip_filter ==
                        SamplerFilter::linear &&
                    device.sampler_descriptions.back().address_u ==
                        SamplerAddressMode::repeat &&
                    device.sampler_descriptions.back().address_v ==
                        SamplerAddressMode::repeat,
                "portable clouds retain immutable geometry, decoded texture, and linear repeat sampler");

        FakeTarget target(request.presentation, backend);
        WorkspaceViewportFrameRequest frame;
        frame.camera = valid_shadow_camera(
            0.0F, backend == Backend::Vulkan ? CameraClipSpace::vulkan
                                              : CameraClipSpace::d3d12);
        const auto lighting = apex::app::evaluateWorkspaceViewportLighting({});
        require(lighting.ok(), "portable cloud lighting evaluates");
        frame.frame_constants = lighting.frame_constants;
        Diagnostic diagnostic;
        require(prepared.viewport->drawAndPresent(device, target, frame,
                                                  diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                    device.events == std::vector<std::string>(
                        {"clouds", "color", "present"}) &&
                    device.cloud_parameters.size() == 1U,
                "portable clouds reach the batch before scene color and present");
        const auto& clouds = device.cloud_parameters.front();
        require(clouds.vertex_buffer != nullptr && clouds.sampler != nullptr &&
                    !clouds.texture_runs.empty() && clouds.textures[0U] != nullptr &&
                    std::any_of(clouds.texture_runs.begin(), clouds.texture_runs.end(),
                                [](const auto& run) { return run.texture != 0U; }),
                "portable cloud batch retains drawable geometry and texture run");
        for (std::size_t index = 1U; index < clouds.textures.size(); ++index)
            require(clouds.textures[index] == nullptr,
                    "missing portable cloud texture slots remain skipped");

        auto missing_constants_request = request_for(value);
        missing_constants_request.portable_clouds =
            portable_cloud_options(texture);
        FakeDevice missing_constants_device(backend);
        auto missing_constants_prepared = apex::app::prepareWorkspaceViewport(
            missing_constants_device, value.document, missing_constants_request);
        require(missing_constants_prepared.ok(),
                "portable cloud missing-constants preparation succeeds");
        FakeTarget missing_constants_target(missing_constants_request.presentation,
                                            backend);
        WorkspaceViewportFrameRequest missing_constants_frame;
        missing_constants_frame.camera = frame.camera;
        require(missing_constants_prepared.viewport->drawAndPresent(
                    missing_constants_device, missing_constants_target,
                    missing_constants_frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::invalid &&
                    diagnostic.code ==
                        "workspace_viewport_portable_cloud_constants_missing" &&
                    missing_constants_device.draw_calls == 0U &&
                    missing_constants_device.present_calls == 0U,
                "portable clouds require current frame constants before drawing");
    }
}

void captures_portable_clouds_on_all_reflection_faces() {
    auto value = fixture();
    configure_multimap_reflection(value);
    const auto texture = portable_cloud_texture_plan();
    auto request = request_for(value);
    request.multimap_reflection = true;
    request.render.include_reflections = true;
    request.render.explicit_reflection_root = 1U;
    request.sky_enabled = true;
    request.portable_reflection_capture =
        apex::app::WorkspaceViewportPortableReflectionCaptureOptions{8U};
    request.portable_clouds = portable_cloud_options(texture);
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "portable cloud reflection preparation succeeds");
    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera = valid_shadow_camera();
    const auto lighting = apex::app::evaluateWorkspaceViewportLighting({});
    require(lighting.ok(), "portable cloud reflection lighting evaluates");
    frame.frame_constants = lighting.frame_constants;
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.cloud_parameters.size() == texture_cube_face_count + 1U,
            "portable clouds draw for each reflection face and the main view");
    std::vector<std::string> expected_events;
    for (std::size_t face = 0U; face < texture_cube_face_count; ++face)
        expected_events.insert(expected_events.end(), {"sky", "clouds", "color"});
    expected_events.insert(expected_events.end(), {"mips", "sky", "clouds",
                                                   "color", "present"});
        require(device.events == expected_events,
            "portable cloud reflection order remains sky, clouds, scene");
    for (std::size_t face = 0U; face < device.cloud_target_subresources.size(); ++face)
        require(device.cloud_target_subresources[face].cube_face ==
                    (face < texture_cube_face_count
                         ? std::array<CubeFace, texture_cube_face_count>{
                               CubeFace::negative_x, CubeFace::positive_x,
                               CubeFace::positive_y, CubeFace::negative_y,
                               CubeFace::positive_z, CubeFace::negative_z}[face]
                         : CubeFace::none),
                "portable cloud reflection uses each target face");
}

void rejects_malformed_portable_cloud_options_atomically() {
    auto value = fixture();
    const auto texture = portable_cloud_texture_plan();
    auto request = request_for(value);
    auto options = portable_cloud_options(texture);
    options.settings.width = std::numeric_limits<float>::quiet_NaN();
    request.portable_clouds = options;
    FakeDevice device;
    const auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(!prepared.ok() && prepared.diagnostic.code ==
                "cloud_settings_invalid" && device.buffer_calls == 0U &&
                device.texture_calls == 0U && device.sampler_calls == 0U,
            "non-finite cloud geometry is rejected before GPU allocation");

    options = portable_cloud_options(texture);
    options.cloud_cover = std::numeric_limits<float>::infinity();
    request.portable_clouds = options;
    FakeDevice lighting_device;
    const auto lighting_prepared = apex::app::prepareWorkspaceViewport(
        lighting_device, value.document, request);
    require(!lighting_prepared.ok() &&
                lighting_prepared.diagnostic.code ==
                    "workspace_viewport_portable_cloud_lighting_invalid" &&
                lighting_device.buffer_calls == 0U &&
                lighting_device.texture_calls == 0U,
            "non-finite cloud lighting is rejected before GPU allocation");
}

void prepares_draws_and_toggles_portable_grass_with_retained_resources() {
    for (const auto backend : {Backend::Vulkan, Backend::D3D12}) {
        auto value = fixture();
        if (backend == Backend::D3D12) {
            for (auto& module : value.modules) {
                module.format = PipelineShaderFormat::dxbc;
                module.bytes = dxbc_shader_bytes();
            }
        }
        const std::array triangles = {portable_grass_triangle()};
        const auto atlas = portable_cloud_texture_plan();
        auto request = request_for(value);
        request.portable_grass = portable_grass_options(triangles, atlas);
        const auto layout = buildPortableGrassLayout(
            request.portable_grass->triangles,
            request.portable_grass->settings,
            request.portable_grass->build);
        require(layout.ready(), "portable grass fixture builds drawable geometry");

        FakeDevice device(backend);
        auto prepared = apex::app::prepareWorkspaceViewport(
            device, value.document, request);
        if (!prepared.ok())
            throw std::runtime_error("portable grass preparation failed: " +
                                     prepared.diagnostic.code + ": " +
                                     prepared.diagnostic.message);
        const auto grass_buffer = std::find_if(
            device.created_buffer_descriptions.begin(),
            device.created_buffer_descriptions.end(), [&](const auto& description) {
                return description.size_bytes ==
                           layout.vertices.size() *
                               portable_grass_vertex_stride_bytes &&
                       description.usage == BufferUsage::vertex &&
                       description.memory == BufferMemory::device_local &&
                       description.mutability == BufferMutability::immutable;
            });
        require(grass_buffer != device.created_buffer_descriptions.end() &&
                    !device.sampler_descriptions.empty() &&
                    device.sampler_descriptions.back().min_filter ==
                        SamplerFilter::linear &&
                    device.sampler_descriptions.back().mag_filter ==
                        SamplerFilter::linear &&
                    device.sampler_descriptions.back().mip_filter ==
                        SamplerFilter::linear &&
                    device.sampler_descriptions.back().address_u ==
                        SamplerAddressMode::repeat &&
                    device.sampler_descriptions.back().address_v ==
                        SamplerAddressMode::repeat,
                "portable grass retains immutable geometry, decoded atlas, and linear repeat sampler");

        FakeTarget target(request.presentation, backend);
        WorkspaceViewportFrameRequest frame;
        frame.camera = valid_shadow_camera(
            0.5F, backend == Backend::Vulkan ? CameraClipSpace::vulkan
                                              : CameraClipSpace::d3d12);
        const auto lighting = apex::app::evaluateWorkspaceViewportLighting({});
        require(lighting.ok(), "portable grass lighting evaluates");
        frame.frame_constants = lighting.frame_constants;
        frame.portable_grass = WorkspaceViewportPortableGrassFrameOptions{
            true, 0.75F, {0.0F, 1.0F}, 2.0F, 3.5F};
        Diagnostic diagnostic;
        require(prepared.viewport->drawAndPresent(device, target, frame,
                                                  diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                    device.events == std::vector<std::string>(
                        {"grass", "color", "present"}) &&
                    device.grass_parameters.size() == 1U,
                "portable grass reaches the batch before scene color and present");
        const auto& grass = device.grass_parameters.front();
        require(grass.vertex_buffer != nullptr && grass.atlas != nullptr &&
                    grass.sampler != nullptr &&
                    grass.vertex_count == layout.vertices.size() &&
                    grass.camera.view_projection == frame.camera.view_projection &&
                    grass.wetness == 0.75F &&
                    grass.wind_direction == std::array<float, 2U>{0.0F, 1.0F} &&
                    grass.wind_strength == 2.0F &&
                    grass.elapsed_seconds == 3.5F &&
                    grass.sun_color[0U] ==
                        lighting.frame_constants.sun_color[0U] &&
                    grass.fog_distance == lighting.frame_constants.fog[0U],
                "portable grass forwards owned resources and current frame values");

        const std::size_t draw_count = device.draw_calls;
        const std::size_t event_count = device.events.size();
        WorkspaceViewportFrameRequest hidden;
        hidden.camera = frame.camera;
        hidden.frame_constants = lighting.frame_constants;
        hidden.portable_grass = WorkspaceViewportPortableGrassFrameOptions{};
        hidden.portable_grass->visible = false;
        require(prepared.viewport->drawAndPresent(device, target, hidden,
                                                  diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                    device.draw_calls == draw_count + 1U &&
                    device.grass_parameters.size() == 1U &&
                    std::vector<std::string>(device.events.begin() +
                                                 static_cast<std::ptrdiff_t>(event_count),
                                             device.events.end()) ==
                        std::vector<std::string>({"color", "present"}),
                "a hidden portable grass frame is a resource-preserving no-op");

        WorkspaceViewportFrameRequest missing_constants;
        missing_constants.camera = frame.camera;
        require(prepared.viewport->drawAndPresent(device, target,
                                                  missing_constants,
                                                  diagnostic) ==
                    WorkspaceViewportFrameStatus::invalid &&
                    diagnostic.code ==
                        "workspace_viewport_portable_grass_constants_missing" &&
                    device.draw_calls == draw_count + 1U,
                "visible portable grass rejects missing frame constants before drawing");

        WorkspaceViewportFrameRequest invalid_frame = frame;
        invalid_frame.portable_grass->wind_strength =
            std::numeric_limits<float>::infinity();
        require(prepared.viewport->drawAndPresent(device, target, invalid_frame,
                                                  diagnostic) ==
                    WorkspaceViewportFrameStatus::invalid &&
                    diagnostic.code == "portable_grass_lighting_invalid" &&
                    device.draw_calls == draw_count + 1U,
                "malformed portable grass frame overrides fail before drawing");
    }
}

void captures_portable_grass_on_all_reflection_faces() {
    auto value = fixture();
    configure_multimap_reflection(value);
    const std::array triangles = {portable_grass_triangle()};
    const auto atlas = portable_cloud_texture_plan();
    auto request = request_for(value);
    request.multimap_reflection = true;
    request.render.include_reflections = true;
    request.render.explicit_reflection_root = 1U;
    request.portable_reflection_capture =
        apex::app::WorkspaceViewportPortableReflectionCaptureOptions{8U};
    request.portable_grass = portable_grass_options(triangles, atlas);
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "portable grass reflection preparation succeeds");
    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera = valid_shadow_camera(0.75F);
    const auto lighting = apex::app::evaluateWorkspaceViewportLighting({});
    require(lighting.ok(), "portable grass reflection lighting evaluates");
    frame.frame_constants = lighting.frame_constants;
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.grass_parameters.size() == texture_cube_face_count + 1U,
            "portable grass draws for each reflection face and the main view");
    std::vector<std::string> expected_events;
    for (std::size_t face = 0U; face < texture_cube_face_count; ++face)
        expected_events.insert(expected_events.end(), {"grass", "color"});
    expected_events.insert(expected_events.end(),
                           {"mips", "grass", "color", "present"});
    require(device.events == expected_events,
            "portable grass reflection order remains grass then scene");
    constexpr std::array faces = {
        CubeFace::negative_x, CubeFace::positive_x, CubeFace::positive_y,
        CubeFace::negative_y, CubeFace::positive_z, CubeFace::negative_z};
    for (std::size_t face = 0U; face < device.grass_parameters.size(); ++face) {
        require(device.grass_target_subresources[face].cube_face ==
                    (face < texture_cube_face_count ? faces[face]
                                                    : CubeFace::none),
                "portable grass uses each reflection target face");
        require(device.grass_parameters[face].elapsed_seconds == 1.5F,
                "portable grass keeps one deterministic time across capture faces");
    }
    require(device.grass_parameters.front().camera.view_projection !=
                frame.camera.view_projection &&
                device.grass_parameters.back().camera.view_projection ==
                    frame.camera.view_projection,
            "static-scene execution replaces portable grass cameras for capture and main frames");
}

void rejects_malformed_portable_grass_options_atomically() {
    auto value = fixture();
    std::array triangles = {portable_grass_triangle()};
    auto atlas = portable_cloud_texture_plan();
    auto request = request_for(value);

    auto options = portable_grass_options(triangles, atlas);
    options.build.declared_triangle_count = 2U;
    request.portable_grass = options;
    FakeDevice truncated_device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        truncated_device, value.document, request);
    require(!prepared.ok() && prepared.diagnostic.code ==
                "grass_source_span_truncated" &&
                truncated_device.buffer_calls == 0U &&
                truncated_device.texture_calls == 0U &&
                truncated_device.sampler_calls == 0U,
            "truncated portable grass source is rejected before GPU allocation");

    triangles[0U].vertices[1U].position[2U] =
        std::numeric_limits<float>::quiet_NaN();
    options = portable_grass_options(triangles, atlas);
    request.portable_grass = options;
    FakeDevice source_device;
    prepared = apex::app::prepareWorkspaceViewport(
        source_device, value.document, request);
    require(!prepared.ok() && prepared.diagnostic.code ==
                "grass_source_vertex_nonfinite" &&
                source_device.buffer_calls == 0U &&
                source_device.texture_calls == 0U,
            "non-finite portable grass source is rejected before GPU allocation");

    triangles = {portable_grass_triangle()};
    options = portable_grass_options(triangles, atlas);
    options.frame.wetness = std::numeric_limits<float>::infinity();
    request.portable_grass = options;
    FakeDevice frame_device;
    prepared = apex::app::prepareWorkspaceViewport(
        frame_device, value.document, request);
    require(!prepared.ok() && prepared.diagnostic.code ==
                "portable_grass_lighting_invalid" &&
                frame_device.buffer_calls == 0U &&
                frame_device.texture_calls == 0U,
            "non-finite portable grass frame values are rejected before GPU allocation");

    options = portable_grass_options(triangles, atlas);
    options.atlas = nullptr;
    request.portable_grass = options;
    FakeDevice missing_atlas_device;
    prepared = apex::app::prepareWorkspaceViewport(
        missing_atlas_device, value.document, request);
    require(!prepared.ok() && prepared.diagnostic.code ==
                "workspace_viewport_portable_grass_atlas_missing" &&
                missing_atlas_device.buffer_calls == 0U &&
                missing_atlas_device.texture_calls == 0U,
            "missing portable grass atlas is rejected before GPU allocation");

    atlas.description.format = TextureFormat::r8_unorm;
    atlas.levels[0U].pixels = {std::uint8_t{255U}};
    options = portable_grass_options(triangles, atlas);
    request.portable_grass = options;
    FakeDevice format_device;
    prepared = apex::app::prepareWorkspaceViewport(
        format_device, value.document, request);
    require(!prepared.ok() && prepared.diagnostic.code ==
                "workspace_viewport_portable_grass_atlas_invalid" &&
                format_device.buffer_calls == 0U &&
                format_device.texture_calls == 0U,
            "unsupported portable grass atlas format is rejected before GPU allocation");

    auto unprepared_request = request_for(value);
    FakeDevice unprepared_device;
    prepared = apex::app::prepareWorkspaceViewport(
        unprepared_device, value.document, unprepared_request);
    require(prepared.ok(), "viewport without portable grass prepares");
    FakeTarget target(unprepared_request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera = valid_shadow_camera();
    frame.portable_grass = WorkspaceViewportPortableGrassFrameOptions{};
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(unprepared_device, target, frame,
                                              diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "workspace_viewport_portable_grass_unprepared" &&
                unprepared_device.draw_calls == 0U,
            "portable grass frame overrides require retained prepared resources");
}

void draws_explicit_multimap_reflection_cube_outside_model_textures() {
    auto value = fixture();
    configure_multimap_reflection(value);
    auto& model = value.document.assembly.model;

    auto request = request_for(value);
    request.multimap_reflection = true;
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok() &&
                prepared.viewport->preparation().resources
                    ->requires_multimap_reflection_cube() &&
                model.textures.size() == 3U,
            "viewport preparation forwards the explicit MultiMap reflection contract without adding a model texture");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    const auto missing = prepared.viewport->drawAndPresent(
        device, target, frame, diagnostic);
    require(missing == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code ==
                    "static_scene_multimap_reflection_cube_missing" &&
                device.draw_calls == 0U && device.present_calls == 0U,
            "viewport reflection frames reject a missing cube before draw and present");

    const TextureDescription cube_description{
        16U, 16U, 1U, 1U, TextureFormat::rgba8_unorm,
        TextureUsage::sampled, TextureMemory::device_local,
        TextureMutability::immutable, 1U, TextureShape::texture_cube};
    FakeTexture cube(cube_description, Backend::Vulkan);
    FakeSampler sampler(SamplerDescription{}, Backend::Vulkan);
    frame.multimap_reflection_cube = {&cube, &sampler};
    const auto drawn = prepared.viewport->drawAndPresent(
        device, target, frame, diagnostic);
    require(drawn == WorkspaceViewportFrameStatus::ready &&
                device.draw_calls == 1U && device.present_calls == 1U &&
                device.reflection_textures ==
                    std::vector<const Texture*>{&cube} &&
                model.textures.size() == 3U,
            "viewport forwards the frame-owned cube while retaining the original KN5 texture table");
}

void captures_and_publishes_six_portable_reflection_faces_atomically() {
    auto value = fixture();
    configure_multimap_reflection(value);
    auto request = request_for(value);
    request.multimap_reflection = true;
    request.render.include_reflections = true;
    request.render.explicit_reflection_root = 1U;
    request.sky_enabled = true;
    request.skeleton_overlay =
        apex::app::WorkspaceViewportSkeletonOverlayOptions{};
    request.authoring_overlay_pipeline = authoring_overlay_pipeline(value);
    request.portable_reflection_capture =
        apex::app::WorkspaceViewportPortableReflectionCaptureOptions{8U};

    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    if (!prepared.ok())
        throw std::runtime_error(
            "portable reflection capture preparation failed: " +
            prepared.diagnostic.code + ": " + prepared.diagnostic.message);

    std::vector<const TextureDescription*> capture_descriptions;
    const TextureDescription* black_description = nullptr;
    for (const auto& description : device.created_texture_descriptions) {
        if (description.shape != TextureShape::texture_cube) continue;
        if (description.access_policy == TextureAccessPolicy::render_then_sample)
            capture_descriptions.push_back(&description);
        else
            black_description = &description;
    }
    require(capture_descriptions.size() == 2U &&
                black_description != nullptr &&
                black_description->width == 1U &&
                black_description->usage == TextureUsage::sampled,
            "capture owns two candidates and one initialized black bootstrap cube");
    for (const TextureDescription* description : capture_descriptions) {
        require(description->width == 8U && description->height == 8U &&
                    description->format == TextureFormat::rgba16_sfloat &&
                    description->mip_levels == 4U &&
                    description->array_layers == 1U &&
                    description->samples == 1U &&
                    description->mutability == TextureMutability::mutable_data &&
                    description->usage ==
                        (TextureUsage::sampled |
                         TextureUsage::color_attachment |
                         TextureUsage::transfer_source),
                "capture candidates use the bounded render-then-sample contract");
    }
    require(!device.created_depth_descriptions.empty() &&
                device.created_depth_descriptions.back().width == 8U &&
                device.created_depth_descriptions.back().height == 8U &&
                device.created_depth_descriptions.back().samples == 1U &&
                !device.sampler_descriptions.empty() &&
                device.sampler_descriptions.back().max_lod == 3.0F,
            "capture owns matching depth and a bounded full-chain sampler");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera = valid_shadow_camera();
    frame.camera.position = {3.0F, 4.0F, 5.0F};
    const auto lighting = apex::app::evaluateWorkspaceViewportLighting({});
    require(lighting.ok(), "reflection portable sky lighting evaluates");
    frame.frame_constants = lighting.frame_constants;
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(device, target, frame,
                                              diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.draw_calls == 7U && device.mip_calls == 1U &&
                device.present_calls == 1U &&
                device.mip_targets.front() == device.draw_targets.front() &&
                device.overlay_counts ==
                    std::vector<std::size_t>({0U, 0U, 0U, 0U, 0U, 0U, 1U}),
            "six completed faces generate mips before the main frame and presentation");

    std::vector<std::string> expected_events;
    for (std::size_t face = 0U; face < texture_cube_face_count; ++face) {
        expected_events.push_back("sky");
        expected_events.push_back("color");
    }
    expected_events.push_back("mips");
    expected_events.push_back("sky");
    expected_events.push_back("color");
    expected_events.push_back("present");
    require(device.events == expected_events &&
                device.sky_parameters.size() == texture_cube_face_count + 1U &&
                device.sky_target_subresources.size() ==
                    texture_cube_face_count + 1U,
            "each reflection face and the main frame draw sky before color");

    constexpr std::array<CubeFace, texture_cube_face_count> expected_faces = {
        CubeFace::negative_x, CubeFace::positive_x, CubeFace::positive_y,
        CubeFace::negative_y, CubeFace::positive_z, CubeFace::negative_z};
    const auto& stock_faces = stockCubemapFaces();
    const Texture* first_candidate = device.draw_targets.front();
    const Texture* bootstrap = device.reflection_textures.front();
    for (std::size_t face = 0U; face < expected_faces.size(); ++face) {
        require(device.draw_targets[face] == first_candidate &&
                    device.target_subresources[face].mip_level == 0U &&
                    device.target_subresources[face].array_layer == 0U &&
                    device.target_subresources[face].cube_face ==
                        expected_faces[face] &&
                    device.resolve_targets[face] == nullptr &&
                    !device.capture_requests[face] &&
                    device.reflection_textures[face] == bootstrap &&
                    bootstrap != first_candidate &&
                    device.draw_camera_frames[face].has_value(),
                "each unpublished capture face targets one stock cube subresource");
        require(device.sky_target_subresources[face].mip_level ==
                    device.target_subresources[face].mip_level &&
                    device.sky_target_subresources[face].array_layer ==
                        device.target_subresources[face].array_layer &&
                    device.sky_target_subresources[face].cube_face ==
                        device.target_subresources[face].cube_face,
                "portable sky uses each reflection face target subresource");
        const CameraFrame& camera = *device.draw_camera_frames[face];
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            require(std::abs(camera.forward[axis] -
                                 static_cast<float>(
                                     stock_faces[face].direction[axis])) <
                            0.0001F &&
                        std::abs(camera.up[axis] -
                                 static_cast<float>(
                                     stock_faces[face].up[axis])) < 0.0001F,
                    "capture cameras preserve recovered face directions and up vectors");
            require(std::abs(device.sky_parameters[face].camera.forward[axis] -
                             camera.forward[axis]) < 0.0001F &&
                        std::abs(device.sky_parameters[face].camera.up[axis] -
                                 camera.up[axis]) < 0.0001F,
                    "portable sky uses each reflection face camera");
        }
    }
    require(device.reflection_textures[6U] == first_candidate,
            "the main draw samples only the fully completed first cube");
    require(device.sky_parameters[6U].camera.view_projection ==
                frame.camera.view_projection,
            "main portable sky restores the viewport camera after captures");

    const std::size_t calls_before_mip_failure = device.draw_calls;
    const std::size_t mips_before_failure = device.mip_calls;
    const std::size_t presents_before_mip_failure = device.present_calls;
    device.fail_mip_call = mips_before_failure + 1U;
    require(prepared.viewport->drawAndPresent(device, target, frame,
                                              diagnostic) ==
                WorkspaceViewportFrameStatus::execution_failed &&
                diagnostic.code == "fake_mip_failed" &&
                device.draw_calls == calls_before_mip_failure + 6U &&
                device.mip_calls == mips_before_failure + 1U &&
                device.present_calls == presents_before_mip_failure,
            "failed mip generation does not draw, present, or publish the main frame");
    device.fail_mip_call = 0U;

    const std::size_t calls_before_main_failure = device.draw_calls;
    const std::size_t presents_before_main_failure = device.present_calls;
    device.fail_draw_call = calls_before_main_failure + 7U;
    require(prepared.viewport->drawAndPresent(device, target, frame,
                                              diagnostic) ==
                WorkspaceViewportFrameStatus::execution_failed &&
                diagnostic.code == "fake_draw_failed" &&
                device.draw_calls == calls_before_main_failure + 7U &&
                device.present_calls == presents_before_main_failure,
            "a failed main draw does not publish or present its completed candidate");
    const Texture* unpublished_candidate =
        device.draw_targets[calls_before_main_failure];
    require(unpublished_candidate != first_candidate &&
                device.reflection_textures[calls_before_main_failure + 6U] ==
                    unpublished_candidate,
            "the failed main draw can sample its candidate without publishing it");

    const std::size_t calls_before_failure = device.draw_calls;
    const std::size_t presents_before_failure = device.present_calls;
    device.fail_draw_call = calls_before_failure + 3U;
    require(prepared.viewport->drawAndPresent(device, target, frame,
                                              diagnostic) ==
                WorkspaceViewportFrameStatus::execution_failed &&
                diagnostic.code == "fake_draw_failed" &&
                device.draw_calls == calls_before_failure + 3U &&
                device.present_calls == presents_before_failure,
            "an incomplete replacement capture is not presented");
    const Texture* failed_candidate =
        device.draw_targets[calls_before_failure];
    require(failed_candidate == unpublished_candidate &&
                device.reflection_textures[calls_before_failure] ==
                    first_candidate &&
                device.reflection_textures.back() == first_candidate,
            "failed replacement faces keep sampling the last published cube");

    device.fail_draw_call = 0U;
    const std::size_t retry_first = device.draw_calls;
    require(prepared.viewport->drawAndPresent(device, target, frame,
                                              diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.draw_calls == retry_first + 7U &&
                device.present_calls == presents_before_failure + 1U &&
                device.draw_targets[retry_first] == failed_candidate &&
                device.reflection_textures[retry_first + 6U] ==
                    failed_candidate,
            "a complete retry atomically replaces the published cube");

    const std::size_t calls_before_present_failure = device.draw_calls;
    const std::size_t presents_before_present_failure = device.present_calls;
    device.fail_present = true;
    require(prepared.viewport->drawAndPresent(device, target, frame,
                                              diagnostic) ==
                WorkspaceViewportFrameStatus::execution_failed &&
                diagnostic.code == "fake_present_failed" &&
                device.draw_calls == calls_before_present_failure + 7U &&
                device.present_calls == presents_before_present_failure + 1U,
            "a presentation failure does not publish its completed candidate");
    const Texture* present_failed_candidate =
        device.draw_targets[calls_before_present_failure];
    require(present_failed_candidate == first_candidate &&
                device.reflection_textures[calls_before_present_failure + 6U] ==
                    present_failed_candidate,
            "the unpresented main draw samples only its pending candidate");

    device.fail_present = false;
    const std::size_t calls_before_publish_probe = device.draw_calls;
    device.fail_draw_call = calls_before_publish_probe + 1U;
    require(prepared.viewport->drawAndPresent(device, target, frame,
                                              diagnostic) ==
                WorkspaceViewportFrameStatus::execution_failed &&
                device.draw_targets[calls_before_publish_probe] ==
                    present_failed_candidate &&
                device.reflection_textures[calls_before_publish_probe] ==
                    failed_candidate,
            "the frame after failed presentation still samples the last published cube");
    device.fail_draw_call = 0U;

    FakeTexture caller_cube(*black_description, Backend::Vulkan);
    FakeSampler caller_sampler(SamplerDescription{}, Backend::Vulkan);
    frame.multimap_reflection_cube = {&caller_cube, &caller_sampler};
    const std::size_t calls_before_override = device.draw_calls;
    require(prepared.viewport->drawAndPresent(device, target, frame,
                                              diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code ==
                    "workspace_viewport_reflection_capture_override_conflict" &&
                device.draw_calls == calls_before_override,
            "viewport-owned capture rejects ambiguous caller override precedence");
}

void rejects_invalid_portable_reflection_capture_options_before_allocation() {
    auto value = fixture();
    configure_multimap_reflection(value);

    auto request = request_for(value);
    request.portable_reflection_capture =
        apex::app::WorkspaceViewportPortableReflectionCaptureOptions{8U};
    FakeDevice missing_pipeline_device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        missing_pipeline_device, value.document, request);
    require(!prepared.ok() &&
                prepared.diagnostic.code ==
                    "workspace_viewport_reflection_capture_pipeline_missing" &&
                missing_pipeline_device.texture_calls == 0U,
            "capture rejects a missing reflection pipeline before allocation");

    request.multimap_reflection = true;
    request.render.include_reflections = true;
    request.portable_reflection_capture->size = 0U;
    FakeDevice invalid_size_device;
    prepared = apex::app::prepareWorkspaceViewport(
        invalid_size_device, value.document, request);
    require(!prepared.ok() &&
                prepared.diagnostic.code ==
                    "workspace_viewport_reflection_capture_size_invalid" &&
                invalid_size_device.texture_calls == 0U,
            "capture rejects a zero size before allocation");

    request.portable_reflection_capture->size = 8U;
    request.color_samples = 4U;
    FakeDevice multisample_device;
    prepared = apex::app::prepareWorkspaceViewport(
        multisample_device, value.document, request);
    require(!prepared.ok() &&
                prepared.status ==
                    apex::app::WorkspaceViewportStatus::unsupported &&
                prepared.diagnostic.code ==
                    "workspace_viewport_reflection_capture_multisample_unsupported" &&
                multisample_device.texture_calls == 0U,
            "capture rejects incompatible multisample pipelines before allocation");
}

void draws_four_sample_viewport_through_retained_resolve() {
    auto value = fixture();
    auto request = request_for(value);
    request.color_samples = 4U;
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok() && device.created_texture_descriptions.size() >= 2U &&
                device.created_texture_descriptions[0].samples == 4U &&
                device.created_texture_descriptions[1].samples == 1U &&
                !device.created_depth_descriptions.empty() &&
                device.created_depth_descriptions.front().samples == 4U,
            "four-sample preparation owns matching color and depth plus a one-sample resolve target");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    const auto first_status = prepared.viewport->drawAndPresent(
        device, target, frame, diagnostic);
    if (first_status != WorkspaceViewportFrameStatus::ready)
        throw std::runtime_error("first four-sample frame: " + diagnostic.code);
    const auto second_status = prepared.viewport->drawAndPresent(
        device, target, frame, diagnostic);
    if (second_status != WorkspaceViewportFrameStatus::ready)
        throw std::runtime_error("second four-sample frame: " + diagnostic.code);
    require(device.draw_targets ==
                    std::vector<Texture*>({device.created_textures[0],
                                           device.created_textures[0]}) &&
                device.resolve_targets ==
                    std::vector<Texture*>({device.created_textures[1],
                                           device.created_textures[1]}) &&
                device.presented_textures ==
                    std::vector<Texture*>({device.created_textures[1],
                                           device.created_textures[1]}) &&
                device.capture_requests == std::vector<bool>({false, false}) &&
                device.events == std::vector<std::string>(
                    {"color", "present", "color", "present"}),
            "four-sample frames reuse one resolve image and present only its single-sample output");
}

void draws_opt_in_hdr_viewport_before_presentation() {
    for (const auto backend : {Backend::Vulkan, Backend::D3D12}) {
        for (const std::uint32_t color_samples : {1U, 4U}) {
            auto value = fixture();
            if (backend == Backend::D3D12) {
                for (auto& module : value.modules) {
                    module.format = PipelineShaderFormat::dxbc;
                    module.bytes = dxbc_shader_bytes();
                }
            }
            auto request = request_for(value);
            request.color_samples = color_samples;
            request.hdr_tone_map = HdrToneMapParameters{};
            request.hdr_tone_map->exposure = 0.75F;
            request.hdr_tone_map->bloom.enabled = true;
            request.hdr_tone_map->bloom.threshold = 7.0F;
            request.hdr_tone_map->bloom.composite_scale = 0.25F;
            FakeDevice device(backend);
            auto prepared = apex::app::prepareWorkspaceViewport(
                device, value.document, request);
            if (!prepared.ok())
                throw std::runtime_error("HDR viewport preparation: " +
                                         prepared.diagnostic.code);

            Texture* scene_target = nullptr;
            Texture* hdr_source = nullptr;
            Texture* display_target = nullptr;
            std::size_t hdr_texture_count = 0U;
            for (std::size_t index = 0U;
                 index < device.created_texture_descriptions.size(); ++index) {
                const auto& description =
                    device.created_texture_descriptions[index];
                if (description.format == TextureFormat::rgba16_sfloat) {
                    ++hdr_texture_count;
                    if (description.samples == color_samples) {
                        scene_target = device.created_textures[index];
                        require(
                            description.mutability ==
                                    TextureMutability::mutable_data &&
                                description.shape ==
                                    TextureShape::texture_2d &&
                                description.mip_levels ==
                                    (color_samples == 1U
                                         ? full_mip_count(
                                               request.presentation.width,
                                               request.presentation.height)
                                         : 1U) &&
                                description.array_layers == 1U &&
                                description.usage ==
                                    (color_samples == 1U
                                         ? TextureUsage::sampled |
                                               TextureUsage::color_attachment |
                                               TextureUsage::transfer_source
                                         : TextureUsage::color_attachment |
                                               TextureUsage::transfer_source) &&
                                description.access_policy ==
                                    (color_samples == 1U
                                         ? TextureAccessPolicy::
                                               render_then_sample
                                         : TextureAccessPolicy::fixed_usage),
                            "HDR scene target uses the exact sample contract");
                    }
                    if (description.samples == 1U) {
                        hdr_source = device.created_textures[index];
                        require(
                            description.mutability ==
                                    TextureMutability::mutable_data &&
                                description.shape ==
                                    TextureShape::texture_2d &&
                                description.mip_levels ==
                                    full_mip_count(request.presentation.width,
                                                   request.presentation.height) &&
                                description.array_layers == 1U &&
                                description.usage ==
                                    (TextureUsage::sampled |
                                     TextureUsage::color_attachment |
                                     TextureUsage::transfer_source) &&
                                description.access_policy ==
                                    TextureAccessPolicy::render_then_sample &&
                                static_cast<std::uint32_t>(
                                    description.usage &
                                    TextureUsage::sampled) != 0U,
                            "HDR tone-map source uses the sampled render "
                            "contract");
                    }
                }
                if (description.format == request.presentation.format &&
                    description.samples == 1U &&
                    description.mutability == TextureMutability::mutable_data &&
                    static_cast<std::uint32_t>(
                        description.usage & TextureUsage::color_attachment) !=
                        0U &&
                    static_cast<std::uint32_t>(description.usage &
                                               TextureUsage::transfer_source) !=
                        0U) {
                    display_target = device.created_textures[index];
                    require(description.usage ==
                                    (TextureUsage::color_attachment |
                                     TextureUsage::transfer_source) &&
                                description.shape ==
                                    TextureShape::texture_2d &&
                                description.mip_levels == 1U &&
                                description.array_layers == 1U &&
                                description.access_policy ==
                                    TextureAccessPolicy::fixed_usage,
                            "HDR display target uses the exact LDR contract");
                }
            }
            require(scene_target != nullptr && hdr_source != nullptr &&
                        display_target != nullptr &&
                        hdr_source != display_target,
                    "HDR preparation owns separate scene and display targets");
            require(
                hdr_texture_count == (color_samples == 1U ? 1U : 2U) &&
                    !device.created_depth_descriptions.empty() &&
                    device.created_depth_descriptions.front().samples ==
                        color_samples,
                "HDR preparation owns the exact color and depth target set");

            FakeTarget target(request.presentation, backend);
            WorkspaceViewportFrameRequest frame;
            frame.camera.clip_space = backend == Backend::Vulkan
                                          ? CameraClipSpace::vulkan
                                          : CameraClipSpace::d3d12;
            frame.frame_constants = KsPerPixelFrameConstants{};
            Diagnostic diagnostic;
            auto status = prepared.viewport->drawAndPresent(device, target,
                                                            frame, diagnostic);
            require(status == WorkspaceViewportFrameStatus::ready &&
                        device.events ==
                            std::vector<std::string>(
                                {"color", "tone_map", "present"}) &&
                        device.luminance_calls == 0U &&
                        device.draw_targets ==
                            std::vector<Texture*>({scene_target}) &&
                        device.resolve_targets ==
                            std::vector<Texture*>(
                                {color_samples == 4U ? hdr_source : nullptr}) &&
                        device.tone_map_sources ==
                            std::vector<Texture*>({hdr_source}) &&
                        device.tone_map_destinations ==
                            std::vector<Texture*>({display_target}) &&
                        device.tone_map_parameters.front().bloom.enabled &&
                        device.tone_map_parameters.front().bloom.threshold ==
                            7.0F &&
                        device.tone_map_parameters.front().bloom.composite_scale ==
                            0.25F &&
                        device.presented_textures ==
                            std::vector<Texture*>({display_target}) &&
                        device.tone_map_parameters.front().exposure == 0.75F,
                    "HDR frame draws, tone maps, and presents in order");

            frame.hdr_tone_map = *request.hdr_tone_map;
            frame.hdr_tone_map->exposure = 1.5F;
            frame.hdr_tone_map->bloom.threshold = 9.0F;
            status = prepared.viewport->drawAndPresent(device, target, frame,
                                                       diagnostic);
            require(status == WorkspaceViewportFrameStatus::ready &&
                        device.tone_map_parameters.back().exposure == 1.5F &&
                        device.tone_map_parameters.back().bloom.enabled &&
                        device.tone_map_parameters.back().bloom.threshold ==
                            9.0F &&
                        device.present_calls == 2U,
                    "HDR frame parameters override the prepared defaults");

            device.tone_map_status = HdrToneMapStatus::execution_failed;
            status = prepared.viewport->drawAndPresent(device, target, frame,
                                                       diagnostic);
            require(status == WorkspaceViewportFrameStatus::execution_failed &&
                        diagnostic.code == "fake_tone_map_failed" &&
                        device.tone_map_calls == 3U &&
                        device.present_calls == 2U,
                    "tone-map failure prevents presentation");

            device.tone_map_status = HdrToneMapStatus::ready;
            device.fail_draw = true;
            status = prepared.viewport->drawAndPresent(device, target, frame,
                                                       diagnostic);
            require(status == WorkspaceViewportFrameStatus::execution_failed &&
                        diagnostic.code == "fake_draw_failed" &&
                        device.tone_map_calls == 3U &&
                        device.present_calls == 2U,
                    "HDR draw failure prevents tone mapping and presentation");

            device.fail_draw = false;
            device.fail_present = true;
            status = prepared.viewport->drawAndPresent(device, target, frame,
                                                       diagnostic);
            require(status == WorkspaceViewportFrameStatus::execution_failed &&
                        diagnostic.code == "fake_present_failed" &&
                        device.tone_map_calls == 4U &&
                        device.present_calls == 3U,
                    "HDR presentation failure preserves the execution status");
        }
    }
}

void draws_automatic_exposure_before_tone_map() {
    for (const auto backend : {Backend::Vulkan, Backend::D3D12}) {
        auto value = fixture();
        if (backend == Backend::D3D12) {
            for (auto& module : value.modules) {
                module.format = PipelineShaderFormat::dxbc;
                module.bytes = dxbc_shader_bytes();
            }
        }
        auto request = request_for(value);
        request.hdr_tone_map = HdrToneMapParameters{};
        request.hdr_tone_map->exposure = 0.75F;
        request.hdr_tone_map->bloom.enabled = true;
        request.hdr_tone_map->bloom.threshold = 7.0F;
        request.hdr_tone_map->bloom.composite_scale = 0.25F;
        request.hdr_exposure_mode = HdrExposureMode::automatic;
        request.color_samples = 4U;
        FakeDevice device(backend);
        auto prepared = apex::app::prepareWorkspaceViewport(
            device, value.document, request);
        if (!prepared.ok())
            throw std::runtime_error("automatic HDR preparation: " +
                                     prepared.diagnostic.code);

        Texture* hdr_source = nullptr;
        for (std::size_t index = 0U;
             index < device.created_texture_descriptions.size(); ++index) {
            const auto& description = device.created_texture_descriptions[index];
            if (description.format == TextureFormat::rgba16_sfloat &&
                description.samples == 1U) {
                hdr_source = device.created_textures[index];
                require(
                    description.mip_levels ==
                        full_mip_count(request.presentation.width,
                                       request.presentation.height),
                    "automatic exposure source owns the complete mip chain");
            }
        }
        require(hdr_source != nullptr, "automatic exposure has an HDR source");

        FakeTarget target(request.presentation, backend);
        WorkspaceViewportFrameRequest frame;
        frame.camera.clip_space = backend == Backend::Vulkan
                                      ? CameraClipSpace::vulkan
                                      : CameraClipSpace::d3d12;
        frame.frame_constants = KsPerPixelFrameConstants{};
        Diagnostic diagnostic;
        const auto status = prepared.viewport->drawAndPresent(
            device, target, frame, diagnostic);
        require(status == WorkspaceViewportFrameStatus::ready &&
                    device.events == std::vector<std::string>(
                        {"color", "measure_exposure", "tone_map", "present"}) &&
                    device.luminance_calls == 1U &&
                    device.luminance_sources == std::vector<Texture*>({hdr_source}) &&
                    device.tone_map_parameters.size() == 1U &&
                    device.tone_map_parameters.front().bloom.enabled &&
                    device.tone_map_parameters.front().bloom.threshold == 7.0F &&
                    device.tone_map_parameters.front().bloom.composite_scale ==
                        0.25F &&
                    std::abs(device.tone_map_parameters.front().exposure - 0.5F) <
                        1e-6F,
                "automatic exposure measures the resolved scene before tone mapping");

        device.events.clear();
        device.fail_luminance = true;
        const auto failed = prepared.viewport->drawAndPresent(
            device, target, frame, diagnostic);
        require(failed == WorkspaceViewportFrameStatus::execution_failed &&
                    diagnostic.code == "fake_luminance_failed" &&
                    device.events == std::vector<std::string>(
                        {"color", "measure_exposure"}) &&
                    device.tone_map_calls == 1U && device.present_calls == 1U,
                "automatic exposure failure prevents tone mapping and presentation");
    }
}

void draws_opt_in_fxaa_after_hdr_postprocessing() {
    for (const auto backend : {Backend::Vulkan, Backend::D3D12}) {
        {
            auto value = fixture();
            auto request = request_for(value);
            request.fxaa_enabled = true;
            FakeDevice device(backend);
            auto rejected = apex::app::prepareWorkspaceViewport(
                device, value.document, request);
            require(!rejected.ok() &&
                        rejected.diagnostic.code ==
                            "workspace_viewport_fxaa_requires_hdr" &&
                        device.texture_calls == 0U,
                    "LDR viewport rejects FXAA before allocation");
        }
        for (const std::uint32_t samples : {1U, 4U}) {
            auto value = fixture();
            if (backend == Backend::D3D12) {
                for (auto &module : value.modules) {
                    module.format = PipelineShaderFormat::dxbc;
                    module.bytes = dxbc_shader_bytes();
                }
            }
            auto request = request_for(value);
            request.color_samples = samples;
            request.fxaa_enabled = true;
            request.hdr_tone_map = HdrToneMapParameters{};
            FakeDevice device(backend);
            auto prepared = apex::app::prepareWorkspaceViewport(
                device, value.document, request);
            if (!prepared.ok())
                throw std::runtime_error("FXAA viewport preparation: " +
                                         prepared.diagnostic.code);

            FakeTarget target(request.presentation, backend);
            WorkspaceViewportFrameRequest frame;
            frame.camera.clip_space = backend == Backend::Vulkan
                                          ? CameraClipSpace::vulkan
                                          : CameraClipSpace::d3d12;
            frame.frame_constants = KsPerPixelFrameConstants{};
            Diagnostic diagnostic;
            const auto status = prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic);
            require(status == WorkspaceViewportFrameStatus::ready &&
                        device.fxaa_calls == 1U &&
                        device.fxaa_sources.size() == 1U &&
                        device.fxaa_destinations.size() == 1U &&
                        device.fxaa_sources.front() !=
                            device.fxaa_destinations.front(),
                    "FXAA viewport runs with distinct source and destination");
            const std::vector<std::string> expected = {
                "color", "tone_map", "fxaa", "present"};
            require(device.events == expected,
                    "FXAA follows HDR tone mapping in order");
            require(device.fxaa_sources.front()->info().description.format ==
                        TextureFormat::rgba8_unorm &&
                    device.fxaa_destinations.front()->info()
                            .description.format == request.presentation.format,
                    "FXAA uses the RGBA8 post-process source and display target");
            require(device.tone_map_destinations.front() ==
                        device.fxaa_sources.front(),
                    "HDR tone mapping feeds the FXAA source");

            const std::size_t present_before_failure = device.present_calls;
            device.fxaa_status = FxaaStatus::execution_failed;
            const auto failed_fxaa = prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic);
            require(failed_fxaa == WorkspaceViewportFrameStatus::execution_failed &&
                        diagnostic.code == "fake_fxaa_failed" &&
                        device.present_calls == present_before_failure,
                    "FXAA failure prevents presentation");

            device.fxaa_status = FxaaStatus::ready;
            const std::size_t fxaa_before_draw_failure = device.fxaa_calls;
            device.fail_draw = true;
            const auto failed_draw = prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic);
            require(failed_draw == WorkspaceViewportFrameStatus::execution_failed &&
                        diagnostic.code == "fake_draw_failed" &&
                        device.fxaa_calls == fxaa_before_draw_failure,
                    "scene draw failure prevents FXAA");
            device.fail_draw = false;

            const std::size_t fxaa_before_tone_map_failure = device.fxaa_calls;
            device.tone_map_status = HdrToneMapStatus::execution_failed;
            const auto failed_tone_map = prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic);
            require(failed_tone_map == WorkspaceViewportFrameStatus::execution_failed &&
                        diagnostic.code == "fake_tone_map_failed" &&
                        device.fxaa_calls == fxaa_before_tone_map_failure,
                    "tone-map failure prevents FXAA");
            device.tone_map_status = HdrToneMapStatus::ready;

            device.fail_present = true;
            const auto failed_present = prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic);
            require(failed_present == WorkspaceViewportFrameStatus::execution_failed &&
                        diagnostic.code == "fake_present_failed" &&
                        device.fxaa_calls > fxaa_before_draw_failure,
                    "presentation failure is reported after FXAA");
        }
    }
}

void reallocates_fxaa_targets_after_viewport_resize() {
    auto value = fixture();
    auto request = request_for(value);
    request.presentation.width = 16U;
    request.presentation.height = 8U;
    request.hdr_tone_map = HdrToneMapParameters{};
    request.fxaa_enabled = true;
    FakeDevice device;
    auto first = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(first.ok(), "initial FXAA viewport preparation succeeds");

    FakeTarget first_target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    require(first.viewport->drawAndPresent(device, first_target, frame,
                                           diagnostic) ==
                WorkspaceViewportFrameStatus::ready,
            "initial FXAA viewport frame succeeds");
    require(device.fxaa_destinations.size() == 1U,
            "initial FXAA destination is recorded");
    Texture *old_destination = device.fxaa_destinations.front();
    require(old_destination->info().description.width == 16U &&
                old_destination->info().description.height == 8U,
            "initial FXAA target has the initial viewport dimensions");

    request.presentation.width = 32U;
    request.presentation.height = 16U;
    auto resized = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(resized.ok(), "resized FXAA viewport preparation succeeds");
    FakeTarget resized_target(request.presentation);
    require(resized.viewport->drawAndPresent(device, resized_target, frame,
                                             diagnostic) ==
                WorkspaceViewportFrameStatus::ready,
            "resized FXAA viewport frame succeeds");
    require(device.fxaa_destinations.size() == 2U &&
                device.fxaa_destinations.back() != old_destination &&
                device.fxaa_destinations.back()->info().description.width ==
                    32U &&
                device.fxaa_destinations.back()->info().description.height ==
                    16U,
            "resize allocates a distinct FXAA target with new dimensions");
    require(old_destination->info().description.width == 16U &&
                old_destination->info().description.height == 8U,
            "resize does not mutate the old prepared FXAA target");
}

void rejects_invalid_viewport_hdr_requests() {
    auto value = fixture();
    auto request = request_for(value);
    FakeDevice device;
    request.hdr_exposure_mode = HdrExposureMode::automatic;
    auto automatic_without_hdr = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(!automatic_without_hdr.ok() &&
                automatic_without_hdr.status ==
                    apex::app::WorkspaceViewportStatus::invalid &&
                automatic_without_hdr.diagnostic.code ==
                    "workspace_viewport_hdr_exposure_requires_hdr" &&
                device.texture_calls == 0U,
            "automatic exposure requires HDR before allocation");

    request = request_for(value);
    request.hdr_exposure_mode = static_cast<HdrExposureMode>(0xffU);
    auto invalid_exposure_mode = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(!invalid_exposure_mode.ok() &&
                invalid_exposure_mode.status ==
                    apex::app::WorkspaceViewportStatus::invalid &&
                invalid_exposure_mode.diagnostic.code ==
                    "workspace_viewport_hdr_exposure_mode_invalid" &&
                device.texture_calls == 0U,
            "unknown exposure mode is rejected before allocation");

    request = request_for(value);
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "LDR viewport preparation succeeds");
    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    frame.hdr_tone_map = HdrToneMapParameters{};
    Diagnostic diagnostic;
    auto status = prepared.viewport->drawAndPresent(
        device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code ==
                    "workspace_viewport_hdr_tone_map_unprepared" &&
                device.draw_calls == 0U && device.tone_map_calls == 0U &&
                device.present_calls == 0U,
            "LDR viewport rejects a frame HDR override before drawing");

    frame.hdr_tone_map.reset();
    frame.hdr_exposure_mode = static_cast<HdrExposureMode>(0xffU);
    status = prepared.viewport->drawAndPresent(device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code ==
                    "workspace_viewport_hdr_exposure_mode_invalid" &&
                device.draw_calls == 0U && device.present_calls == 0U,
            "unknown frame exposure mode is rejected before drawing");

    frame.hdr_exposure_mode = HdrExposureMode::automatic;
    status = prepared.viewport->drawAndPresent(device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "workspace_viewport_hdr_exposure_requires_hdr" &&
                device.draw_calls == 0U && device.present_calls == 0U,
            "LDR viewport rejects a frame automatic-exposure override before drawing");

    request = request_for(value);
    request.hdr_tone_map = HdrToneMapParameters{};
    request.hdr_tone_map->gamma =
        std::numeric_limits<float>::quiet_NaN();
    FakeDevice invalid_device;
    auto invalid = apex::app::prepareWorkspaceViewport(
        invalid_device, value.document, request);
    require(!invalid.ok() &&
                invalid.diagnostic.code == "hdr_tone_map_parameters_invalid" &&
                invalid_device.texture_calls == 0U,
            "invalid HDR parameters fail before texture allocation");

}

void draws_builtin_vulkan_source_through_hdr_tone_map() {
    auto value = fixture();
    auto request = request_for(value);
    request.shader_modules = {};
    request.color_samples = 4U;
    request.hdr_tone_map = HdrToneMapParameters{};
    request.builtin_vulkan_source =
        BuiltinVulkanStockSourceSelector::ks_per_pixel;
    request.directional_shadows =
        apex::app::WorkspaceViewportDirectionalShadowOptions{};
    request.directional_shadows->maps.lighting.map_size = 32U;
    request.directional_shadows->opaque_pipeline =
        opaque_shadow_pipeline(value);

    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    if (!prepared.ok())
        throw std::runtime_error("HDR stock source preparation: " +
                                 prepared.diagnostic.code);
    require(prepared.viewport->preparation().resources != nullptr &&
                prepared.viewport->preparation().resources
                        ->stock_vulkan_source_program_count() == 1U,
            "HDR viewport retains the Vulkan source owner");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera = valid_shadow_camera();
    const auto evaluated = apex::app::evaluateWorkspaceViewportLighting({});
    require(evaluated.ok(), "HDR stock source weather evaluates");
    const auto native_lighting =
        apex::app::buildWorkspaceViewportStockVulkanSourceLighting(
            evaluated.evaluated, request.presentation.width,
            request.presentation.height);
    require(native_lighting.has_value(), "HDR stock source lighting builds");
    frame.frame_constants = evaluated.frame_constants;
    frame.stock_vulkan_source_frame =
        apex::app::buildWorkspaceViewportStockVulkanSourceFrame(
            frame.camera, *native_lighting);
    Diagnostic diagnostic;
    const auto status = prepared.viewport->drawAndPresent(
        device, target, frame, diagnostic);
    if (status != WorkspaceViewportFrameStatus::ready)
        throw std::runtime_error("HDR stock source draw: " + diagnostic.code);
    require(device.events == std::vector<std::string>(
                {"shadow", "shadow", "shadow", "color", "tone_map",
                 "present"}) &&
                device.draw_targets.size() == 1U &&
                device.draw_targets.front()->info().description.format ==
                    TextureFormat::rgba16_sfloat &&
                device.resolve_targets.size() == 1U &&
                device.resolve_targets.front() != nullptr &&
                device.resolve_targets.front()->info().description.format ==
                    TextureFormat::rgba16_sfloat &&
                device.tone_map_sources == device.resolve_targets &&
                device.tone_map_destinations.size() == 1U &&
                device.tone_map_destinations.front()->info()
                        .description.format == request.presentation.format &&
                device.presented_textures ==
                    device.tone_map_destinations,
            "retained Vulkan stock rendering resolves RGBA16F before tone mapping");
}

void prepares_retains_and_toggles_recovered_skeleton_overlay() {
    for (const auto backend : {Backend::Vulkan, Backend::D3D12}) {
        auto value = fixture();
        if (backend == Backend::D3D12) {
            for (auto& module : value.modules) {
                module.format = PipelineShaderFormat::dxbc;
                module.bytes = dxbc_shader_bytes();
            }
        }
        auto& nodes = value.document.scene.snapshot.nodes;
        nodes[0U].transform[12U] = 10.0F;
        nodes[1U].transform[12U] = 1.0F;
        nodes[1U].transform[13U] = 2.0F;
        nodes[1U].transform[14U] = 3.0F;
        nodes[2U].transform[12U] = 4.0F;
        nodes[2U].transform[13U] = 5.0F;
        nodes[2U].transform[14U] = 6.0F;

        auto request = request_for(value);
        request.hdr_tone_map = HdrToneMapParameters{};
        request.packets.selected_node = 1U;
        request.skeleton_overlay =
            apex::app::WorkspaceViewportSkeletonOverlayOptions{};
        request.authoring_overlay_pipeline = authoring_overlay_pipeline(value);
        request.authoring_overlay_pipeline->targets.colors[0U].format =
            PipelineRenderTargetFormat::rgba16_float;
        FakeDevice device(backend);
        auto prepared = apex::app::prepareWorkspaceViewport(
            device, value.document, request);
        if (!prepared.ok()) {
            throw std::runtime_error("skeleton viewport preparation: " +
                                     prepared.diagnostic.code);
        }

        FakeTarget target(request.presentation, backend);
        WorkspaceViewportFrameRequest frame;
        frame.camera.clip_space = backend == Backend::Vulkan
                                      ? CameraClipSpace::vulkan
                                      : CameraClipSpace::d3d12;
        frame.camera.view_projection[5U] = 0.75F;
        frame.frame_constants = KsPerPixelFrameConstants{};
        Diagnostic diagnostic;
        require(prepared.viewport->drawAndPresent(
                    device, target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                    device.overlay_counts ==
                        std::vector<std::size_t>({2U}) &&
                    device.overlay_scene_positions ==
                        std::vector<std::uint32_t>({
                            std::numeric_limits<std::uint32_t>::max(),
                            std::numeric_limits<std::uint32_t>::max()}) &&
                    !device.overlay_depth_tests[0U] &&
                    !device.overlay_depth_writes[0U] &&
                    device.overlay_matrices[0U].world ==
                        apex::scene::identity_matrix &&
                    device.overlay_matrices[0U].view_projection ==
                        frame.camera.view_projection,
                "retained skeleton draws before selection with recovered depth state");

        const auto* skeleton_buffer =
            dynamic_cast<const FakeBuffer*>(device.overlay_buffers[0U]);
        require(skeleton_buffer != nullptr &&
                    skeleton_buffer->info().description.mutability ==
                        BufferMutability::immutable &&
                    device.overlay_vertex_counts[0U] == 14U &&
                    skeleton_buffer->bytes().size() ==
                        14U * sizeof(OverlayLineVertex),
                "skeleton geometry is retained in one immutable backend buffer");
        std::array<OverlayLineVertex, 14U> vertices{};
        std::memcpy(vertices.data(), skeleton_buffer->bytes().data(),
                    skeleton_buffer->bytes().size());
        require(vertices[0U].position ==
                        std::array<float, 3U>{10.03F, 0.0F, 0.0F} &&
                    vertices[0U].color ==
                        apex::render::skeleton_overlay_marker_color &&
                    vertices[12U].position ==
                        std::array<float, 3U>{1.0F, 2.0F, 3.0F} &&
                    vertices[13U].position ==
                        std::array<float, 3U>{4.0F, 5.0F, 6.0F} &&
                    vertices[12U].color ==
                        apex::render::skeleton_overlay_selected_connector_color &&
                    vertices[13U].color ==
                        apex::render::skeleton_overlay_selected_connector_color,
                "viewport preserves recovered world positions and selected connector color");

        frame.skeleton_overlay_visible = false;
        require(prepared.viewport->drawAndPresent(
                    device, target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                    device.overlay_counts ==
                        std::vector<std::size_t>({2U, 1U}),
                "frame override hides the retained skeleton without rebuilding it");
    }
}

void rejects_malformed_skeleton_viewport_inputs_before_allocation() {
    auto value = fixture();
    auto request = request_for(value);
    request.skeleton_overlay =
        apex::app::WorkspaceViewportSkeletonOverlayOptions{};
    request.authoring_overlay_pipeline = authoring_overlay_pipeline(value);

    value.document.scene.snapshot.nodes[1U].transform[0U] =
        std::numeric_limits<float>::quiet_NaN();
    FakeDevice non_finite_device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        non_finite_device, value.document, request);
    require(!prepared.ok() &&
                prepared.diagnostic.code ==
                    "workspace_viewport_skeleton_transform_non_finite" &&
                non_finite_device.texture_calls == 0U &&
                non_finite_device.depth_calls == 0U &&
                non_finite_device.buffer_calls == 0U,
            "non-finite skeleton matrices fail before GPU allocation");

    value.document.scene.snapshot.nodes[1U].transform[0U] = 1.0F;
    value.document.scene.snapshot.nodes[0U].children[0U] = 99U;
    FakeDevice truncated_device;
    prepared = apex::app::prepareWorkspaceViewport(
        truncated_device, value.document, request);
    require(!prepared.ok() &&
                prepared.diagnostic.code ==
                    "workspace_viewport_skeleton_child_truncated" &&
                truncated_device.texture_calls == 0U &&
                truncated_device.depth_calls == 0U &&
                truncated_device.buffer_calls == 0U,
            "truncated skeleton hierarchy fails before GPU allocation");

    auto ordinary_value = fixture();
    auto ordinary_request = request_for(ordinary_value);
    FakeDevice ordinary_device;
    prepared = apex::app::prepareWorkspaceViewport(
        ordinary_device, ordinary_value.document, ordinary_request);
    require(prepared.ok(), "ordinary viewport prepares without skeleton resources");
    FakeTarget target(ordinary_request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    frame.skeleton_overlay_visible = true;
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(
                ordinary_device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "workspace_viewport_skeleton_unprepared" &&
                ordinary_device.draw_calls == 0U,
            "frame override cannot enable an unprepared skeleton overlay");
}

void draws_selected_axis_inside_the_scene_batch() {
    auto value = fixture();
    auto request = request_for(value);
    request.packets.selected_node = 1U;
    request.authoring_overlay_pipeline = authoring_overlay_pipeline(value);
    request.grid_visible = true;
    value.document.scene.snapshot.nodes[1U].transform[12] = 2.0F;
    FakeDevice device;
    auto prepared =
        apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(prepared.ok(), "selected-axis viewport preparation succeeds");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.camera.view_projection[0] = 0.5F;
    frame.frame_constants = KsPerPixelFrameConstants{};
    frame.selection_axis_world = value.document.scene.snapshot.nodes[1U].transform;
    frame.selection_axis_world->at(12) = 3.0F;
    Diagnostic diagnostic;
    const auto status = prepared.viewport->drawAndPresent(
        device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::ready &&
                device.draw_calls == 1U && device.present_calls == 1U &&
                device.overlay_counts == std::vector<std::size_t>({2U}) &&
                device.events ==
                    std::vector<std::string>({"color", "present"}),
            "grid and selection axis share the scene draw batch");
    require(device.overlay_matrices.size() == 2U &&
                device.overlay_matrices[1].world ==
                    apex::scene::identity_matrix &&
                device.overlay_matrices[1].view_projection ==
                    frame.camera.view_projection,
            "axis draw uses world-space vertices and the frame camera");
    const auto axis_update = std::find_if(
        device.buffer_updates.begin(), device.buffer_updates.end(),
        [](const FakeDevice::BufferUpdate& update) {
            return update.bytes.size() == 6U * sizeof(OverlayLineVertex);
        });
    require(axis_update != device.buffer_updates.end(),
            "axis uploads exactly six position-color vertices");
    require(device.overlay_buffers.size() == 2U &&
                device.overlay_buffers[1] == axis_update->buffer,
            "recovered grid is ordered before the selected-node axis");
    std::array<OverlayLineVertex, 6U> vertices{};
    std::memcpy(vertices.data(), axis_update->bytes.data(),
                sizeof(vertices));
    require(vertices[0].position[0] == 3.0F &&
                vertices[1].position[0] == 4.0F &&
                vertices[5].position[2] == -1.0F,
            "animated frame override rebuilds normalized RGB axis geometry");
}

void draws_and_toggles_recovered_world_view_axis() {
    auto value = fixture();
    auto request = request_for(value);
    request.authoring_overlay_pipeline = authoring_overlay_pipeline(value);
    request.view_axis_visible = true;
    request.grid_visible = true;
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "view-axis viewport preparation succeeds");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.camera.view_projection[5] = 0.75F;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.overlay_counts == std::vector<std::size_t>({2U}),
            "view axis and grid share one scene batch");
    require(device.overlay_scene_positions ==
                std::vector<std::uint32_t>({
                    1U, std::numeric_limits<std::uint32_t>::max()}) &&
                device.overlay_buffers.size() == 2U &&
                device.overlay_buffers[0] != device.overlay_buffers[1],
            "view axis executes after opaque geometry and before the late grid");
    require(device.overlay_matrices[0].world == apex::scene::identity_matrix &&
                device.overlay_matrices[0].view_projection ==
                    frame.camera.view_projection,
            "view axis uses an identity world matrix and the frame camera");

    frame.view_axis_visible = false;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.overlay_counts ==
                    std::vector<std::size_t>({2U, 1U}) &&
                device.overlay_scene_positions.back() ==
                    std::numeric_limits<std::uint32_t>::max(),
            "frame override hides the prepared view axis without hiding the grid");
}

void draws_raw_ai_spline_in_recovered_scene_phase() {
    auto value = fixture();
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points.resize(3U);
    spline.points[0].position = {-1.0F, 0.0F, 0.0F};
    spline.points[1].position = {0.0F, 0.0F, 0.0F};
    spline.points[2].position = {1.0F, 0.0F, 0.0F};
    const auto geometry = apex::app::buildWorkspaceAiSplineGeometry(spline);
    require(geometry.ok(), "raw AI spline fixture converts");

    auto ai_only_request = request_for(value);
    ai_only_request.ai_spline_geometry = &geometry.geometry;
    ai_only_request.ai_spline_pipeline = ai_spline_pipeline(value);
    FakeDevice ai_only_device;
    auto ai_only_prepared = apex::app::prepareWorkspaceViewport(
        ai_only_device, value.document, ai_only_request);
    require(ai_only_prepared.ok(),
            "AI spline does not require unrelated authoring overlays");
    FakeTarget ai_only_target(ai_only_request.presentation);
    WorkspaceViewportFrameRequest ai_only_frame;
    ai_only_frame.camera.clip_space = CameraClipSpace::vulkan;
    ai_only_frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic ai_only_diagnostic;
    require(ai_only_prepared.viewport->drawAndPresent(
                ai_only_device, ai_only_target, ai_only_frame,
                ai_only_diagnostic) == WorkspaceViewportFrameStatus::ready &&
                ai_only_device.overlay_counts ==
                    std::vector<std::size_t>({1U}),
            "AI-only viewport submits one recovered raw spline draw");

    auto request = request_for(value);
    request.ai_spline_geometry = &geometry.geometry;
    request.ai_spline_pipeline = ai_spline_pipeline(value);
    request.authoring_overlay_pipeline = authoring_overlay_pipeline(value);
    request.view_axis_visible = true;
    request.grid_visible = true;
    FakeDevice device;
    auto prepared =
        apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(prepared.ok(), "raw AI spline viewport preparation succeeds");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.camera.view_projection[0] = 0.5F;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    require(
        prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
            WorkspaceViewportFrameStatus::ready,
        "raw AI spline frame draws");
    require(device.overlay_counts == std::vector<std::size_t>({3U}) &&
                device.overlay_scene_positions ==
                    std::vector<std::uint32_t>(
                        {1U, 1U, std::numeric_limits<std::uint32_t>::max()}),
            "AI spline precedes the view axis and late grid at the recovered "
            "boundary");
    require(
        device.overlay_vertex_offsets[0] == 0U &&
            device.overlay_vertex_counts[0] == 4U &&
            device.overlay_depth_tests[0] && device.overlay_depth_writes[0] &&
            !device.overlay_depth_tests[1] && !device.overlay_depth_writes[1],
        "AI spline keeps normal depth while authoring overlays stay depth-off");
    require(device.overlay_matrices[0].world == apex::scene::identity_matrix &&
                device.overlay_matrices[0].view_projection ==
                    frame.camera.view_projection &&
                device.overlay_buffers[0] != device.overlay_buffers[1] &&
                device.overlay_buffers[1] != device.overlay_buffers[2],
            "AI spline owns immutable geometry and uses the frame camera");

    apex::formats::AiSpline maximum_spline;
    maximum_spline.version = 7U;
    maximum_spline.points.resize(
        max_overlay_line_total_vertices / 2U + 1U);
    const auto maximum_geometry =
        apex::app::buildWorkspaceAiSplineGeometry(maximum_spline);
    require(maximum_geometry.ok() &&
                maximum_geometry.geometry.chunks.size() ==
                    max_overlay_line_draws,
            "maximum AI spline fixture consumes the complete line budget");
    auto over_budget_request = request_for(value);
    over_budget_request.ai_spline_geometry = &maximum_geometry.geometry;
    over_budget_request.ai_spline_pipeline = ai_spline_pipeline(value);
    over_budget_request.authoring_overlay_pipeline =
        authoring_overlay_pipeline(value);
    over_budget_request.grid_visible = true;
    FakeDevice over_budget_device;
    const auto over_budget = apex::app::prepareWorkspaceViewport(
        over_budget_device, value.document, over_budget_request);
    require(!over_budget.ok() &&
                over_budget.diagnostic.code ==
                    "workspace_viewport_overlay_budget_exceeded",
            "AI spline and authoring overlays share one bounded budget");

    apex::formats::AiSpline curved_spline;
    curved_spline.version = 7U;
    curved_spline.points.resize(4U);
    curved_spline.points[0].position = {0.0F, 0.0F, 0.0F};
    curved_spline.points[1].position = {100.0F, 0.0F, 0.0F};
    curved_spline.points[2].position = {200.0F, 100.0F, 0.0F};
    curved_spline.points[3].position = {300.0F, 100.0F, 0.0F};
    curved_spline.payloads.resize(curved_spline.points.size());
    const auto interpolated = apex::app::buildWorkspaceAiSplineGeometry(
        curved_spline,
        apex::app::WorkspaceAiSplineDisplayMode::interpolated);
    require(interpolated.ok(), "interpolated AI spline fixture converts");
    auto interpolated_request = request_for(value);
    interpolated_request.ai_spline_geometry = &interpolated.geometry;
    interpolated_request.ai_spline_pipeline = ai_spline_pipeline(value);
    FakeDevice interpolated_device;
    auto interpolated_prepared = apex::app::prepareWorkspaceViewport(
        interpolated_device, value.document, interpolated_request);
    require(interpolated_prepared.ok(),
            "interpolated AI spline viewport preparation succeeds");
    FakeTarget interpolated_target(interpolated_request.presentation);
    WorkspaceViewportFrameRequest interpolated_frame;
    interpolated_frame.camera.clip_space = CameraClipSpace::vulkan;
    interpolated_frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic interpolated_diagnostic;
    require(interpolated_prepared.viewport->drawAndPresent(
                interpolated_device, interpolated_target, interpolated_frame,
                interpolated_diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                interpolated_device.overlay_counts ==
                    std::vector<std::size_t>({3U}) &&
                interpolated_device.overlay_depth_tests ==
                    std::vector<bool>({true, true, true}) &&
                interpolated_device.overlay_depth_writes ==
                    std::vector<bool>({true, true, true}),
            "interpolated AI spline keeps the recovered pass and depth state");

    auto forged_geometry = interpolated.geometry;
    forged_geometry.sample_point_count = 5'000U;
    auto forged_request = request_for(value);
    forged_request.ai_spline_geometry = &forged_geometry;
    forged_request.ai_spline_pipeline = ai_spline_pipeline(value);
    FakeDevice forged_device;
    const auto forged = apex::app::prepareWorkspaceViewport(
        forged_device, value.document, forged_request);
    require(!forged.ok() &&
                forged.diagnostic.code ==
                    "workspace_viewport_ai_spline_geometry_invalid",
            "viewport rejects forged interpolated spline metadata");

    const auto interval =
        apex::app::buildWorkspaceAiSplineIntervalGeometry(
            curved_spline, {0.25F, 0.2504F});
    require(interval.ok(), "interpolated interval fixture converts");
    const auto primary =
        apex::app::buildWorkspaceAiSplineGeometry(curved_spline);
    const std::array<std::uint32_t, 2U> interval_selected = {0U, 1U};
    const auto interval_selection =
        apex::app::buildWorkspaceAiSplineSelectionGeometry(
            curved_spline, interval_selected);
    require(primary.ok() && interval_selection.ok(),
            "interval and selection fixtures convert");
    auto interval_request = request_for(value);
    interval_request.ai_spline_geometry = &primary.geometry;
    interval_request.ai_spline_pipeline = ai_spline_pipeline(value);
    interval_request.ai_spline_interval_geometry = &interval.geometry;
    interval_request.ai_spline_interval_pipeline =
        ai_spline_interval_pipeline(value);
    interval_request.ai_spline_selection_geometry =
        &interval_selection.geometry;
    interval_request.ai_spline_selection_pipeline =
        ai_spline_camber_pipeline(value);
    FakeDevice interval_device;
    auto interval_prepared = apex::app::prepareWorkspaceViewport(
        interval_device, value.document, interval_request);
    require(interval_prepared.ok(),
            "primary and interval AI spline passes prepare together");
    FakeTarget interval_target(interval_request.presentation);
    WorkspaceViewportFrameRequest interval_frame;
    interval_frame.camera.clip_space = CameraClipSpace::vulkan;
    interval_frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic interval_diagnostic;
    require(interval_prepared.viewport->drawAndPresent(
                interval_device, interval_target, interval_frame,
                interval_diagnostic) == WorkspaceViewportFrameStatus::ready &&
                interval_device.overlay_counts ==
                    std::vector<std::size_t>({3U}) &&
                interval_device.overlay_vertex_counts ==
                    std::vector<std::uint32_t>({6U, 4U, 4U}) &&
                interval_device.overlay_depth_tests ==
                    std::vector<bool>({true, false, true}) &&
                interval_device.overlay_depth_writes ==
                    std::vector<bool>({true, false, true}) &&
                interval_device.overlay_buffers[0] !=
                    interval_device.overlay_buffers[1],
            "selection restores normal depth after the blue interval");

    auto wrong_depth_request = interval_request;
    wrong_depth_request.ai_spline_interval_pipeline =
        ai_spline_pipeline(value);
    FakeDevice wrong_depth_device;
    const auto wrong_depth = apex::app::prepareWorkspaceViewport(
        wrong_depth_device, value.document, wrong_depth_request);
    require(!wrong_depth.ok() &&
                wrong_depth.diagnostic.code ==
                    "workspace_viewport_ai_spline_depth_invalid",
            "interval pass rejects a normal-depth pipeline");

    auto forged_interval = interval.geometry;
    forged_interval.vertices.front().color =
        apex::app::workspace_ai_spline_raw_color;
    auto wrong_color_request = interval_request;
    wrong_color_request.ai_spline_interval_geometry = &forged_interval;
    FakeDevice wrong_color_device;
    const auto wrong_color = apex::app::prepareWorkspaceViewport(
        wrong_color_device, value.document, wrong_color_request);
    require(!wrong_color.ok() &&
                wrong_color.diagnostic.code ==
                    "workspace_viewport_ai_spline_vertex_invalid",
            "interval pass rejects a forged non-blue vertex");

    auto missing_primary_request = request_for(value);
    missing_primary_request.ai_spline_interval_geometry = &interval.geometry;
    missing_primary_request.ai_spline_interval_pipeline =
        ai_spline_interval_pipeline(value);
    FakeDevice missing_primary_device;
    const auto missing_primary = apex::app::prepareWorkspaceViewport(
        missing_primary_device, value.document, missing_primary_request);
    require(!missing_primary.ok() &&
                missing_primary.diagnostic.code ==
                    "workspace_viewport_ai_spline_overlay_primary_missing",
            "AI spline overlays cannot be detached from the primary spline");

    auto interval_budget_request = request_for(value);
    interval_budget_request.ai_spline_geometry = &maximum_geometry.geometry;
    interval_budget_request.ai_spline_pipeline = ai_spline_pipeline(value);
    interval_budget_request.ai_spline_interval_geometry = &interval.geometry;
    interval_budget_request.ai_spline_interval_pipeline =
        ai_spline_interval_pipeline(value);
    FakeDevice interval_budget_device;
    const auto interval_budget = apex::app::prepareWorkspaceViewport(
        interval_budget_device, value.document, interval_budget_request);
    require(!interval_budget.ok() &&
                interval_budget.diagnostic.code ==
                    "workspace_viewport_ai_spline_limit",
            "primary and interval passes share the bounded AI spline budget");
}

void draws_recovered_ai_spline_side_passes() {
    auto value = fixture();
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points.resize(4U);
    spline.points[0].position = {0.0F, 0.0F, 0.0F};
    spline.points[1].position = {100.0F, 0.0F, 0.0F};
    spline.points[2].position = {200.0F, 0.0F, 100.0F};
    spline.points[3].position = {300.0F, 0.0F, 100.0F};
    spline.payloads.resize(spline.points.size());
    for (auto& payload : spline.payloads) {
        payload.side0 = 4.0F;
        payload.side1 = 6.0F;
    }
    const auto left = apex::app::buildWorkspaceAiSplineSideGeometry(
        spline, apex::app::WorkspaceAiSplineSide::left);
    const auto right = apex::app::buildWorkspaceAiSplineSideGeometry(
        spline, apex::app::WorkspaceAiSplineSide::right);
    require(left.ok() && right.ok(), "side AI spline fixtures convert");
    require(left.geometry.pass ==
                    apex::app::WorkspaceAiSplinePassKind::left_side &&
                right.geometry.pass ==
                    apex::app::WorkspaceAiSplinePassKind::right_side &&
                left.geometry.mode ==
                    apex::app::WorkspaceAiSplineDisplayMode::raw &&
                right.geometry.mode ==
                    apex::app::WorkspaceAiSplineDisplayMode::raw,
            "side passes retain raw left/right metadata");
    require(left.geometry.vertices.size() == 6U &&
                right.geometry.vertices.size() == 6U,
            "side passes retain all source segments");
    for (const auto& vertex : left.geometry.vertices)
        require(vertex.color == apex::app::workspace_ai_spline_side_color,
                "left side uses the recovered cyan color");
    for (const auto& vertex : right.geometry.vertices)
        require(vertex.color == apex::app::workspace_ai_spline_side_color,
                "right side uses the recovered cyan color");

    const auto primary = apex::app::buildWorkspaceAiSplineGeometry(spline);
    require(primary.ok(), "side viewport primary fixture converts");
    auto request = request_for(value);
    request.ai_spline_geometry = &primary.geometry;
    request.ai_spline_pipeline = ai_spline_pipeline(value);
    request.ai_spline_left_geometry = &left.geometry;
    request.ai_spline_left_pipeline = ai_spline_side_pipeline(value);
    request.ai_spline_right_geometry = &right.geometry;
    request.ai_spline_right_pipeline = ai_spline_side_pipeline(value);
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "left and right side passes prepare");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.overlay_counts == std::vector<std::size_t>({3U}) &&
                device.overlay_vertex_counts ==
                    std::vector<std::uint32_t>({6U, 6U, 6U}) &&
                device.overlay_depth_tests ==
                    std::vector<bool>({true, true, true}) &&
                device.overlay_depth_writes ==
                    std::vector<bool>({true, true, true}),
            "side passes draw after the primary in normal depth mode");
    require(device.overlay_buffers.size() == 3U &&
                device.overlay_buffers[0] != device.overlay_buffers[1] &&
                device.overlay_buffers[1] != device.overlay_buffers[2] &&
                device.overlay_buffers[0] != device.overlay_buffers[2],
            "primary, left, and right side passes retain submission order");

    auto missing_primary_request = request_for(value);
    missing_primary_request.ai_spline_left_geometry = &left.geometry;
    missing_primary_request.ai_spline_left_pipeline =
        ai_spline_side_pipeline(value);
    FakeDevice missing_primary_device;
    const auto missing_primary = apex::app::prepareWorkspaceViewport(
        missing_primary_device, value.document, missing_primary_request);
    require(!missing_primary.ok() &&
                missing_primary.diagnostic.code ==
                    "workspace_viewport_ai_spline_overlay_primary_missing",
            "side pass cannot be detached from the primary spline");

    auto wrong_color_geometry = left.geometry;
    wrong_color_geometry.vertices.front().color =
        apex::app::workspace_ai_spline_raw_color;
    auto wrong_color_request = request;
    wrong_color_request.ai_spline_left_geometry = &wrong_color_geometry;
    FakeDevice wrong_color_device;
    const auto wrong_color = apex::app::prepareWorkspaceViewport(
        wrong_color_device, value.document, wrong_color_request);
    require(!wrong_color.ok() &&
                wrong_color.diagnostic.code ==
                    "workspace_viewport_ai_spline_vertex_invalid",
            "side pass rejects a non-cyan vertex");

    auto wrong_depth_request = request;
    wrong_depth_request.ai_spline_left_pipeline =
        ai_spline_interval_pipeline(value);
    FakeDevice wrong_depth_device;
    const auto wrong_depth = apex::app::prepareWorkspaceViewport(
        wrong_depth_device, value.document, wrong_depth_request);
    require(!wrong_depth.ok() &&
                wrong_depth.diagnostic.code ==
                    "workspace_viewport_ai_spline_depth_invalid",
            "side pass rejects a depth-off pipeline");

    auto wrong_pass_geometry = left.geometry;
    wrong_pass_geometry.pass = apex::app::WorkspaceAiSplinePassKind::right_side;
    auto wrong_pass_request = request;
    wrong_pass_request.ai_spline_left_geometry = &wrong_pass_geometry;
    FakeDevice wrong_pass_device;
    const auto wrong_pass = apex::app::prepareWorkspaceViewport(
        wrong_pass_device, value.document, wrong_pass_request);
    require(!wrong_pass.ok() &&
                wrong_pass.diagnostic.code ==
                    "workspace_viewport_ai_spline_geometry_invalid",
            "left side input rejects right-side metadata");

    apex::formats::AiSpline maximum_spline;
    maximum_spline.version = 7U;
    maximum_spline.points.resize(
        max_overlay_line_total_vertices / 2U + 1U);
    const auto maximum_geometry =
        apex::app::buildWorkspaceAiSplineGeometry(maximum_spline);
    require(maximum_geometry.ok(), "maximum primary fixture converts");
    auto over_budget_request = request_for(value);
    over_budget_request.ai_spline_geometry = &maximum_geometry.geometry;
    over_budget_request.ai_spline_pipeline = ai_spline_pipeline(value);
    over_budget_request.ai_spline_left_geometry = &left.geometry;
    over_budget_request.ai_spline_left_pipeline =
        ai_spline_side_pipeline(value);
    FakeDevice over_budget_device;
    const auto over_budget = apex::app::prepareWorkspaceViewport(
        over_budget_device, value.document, over_budget_request);
    require(!over_budget.ok() &&
                over_budget.diagnostic.code ==
                    "workspace_viewport_ai_spline_limit",
            "side passes share the bounded AI spline aggregate budget");
}

void draws_recovered_ai_spline_camber_pass() {
    auto value = fixture();
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points.resize(3U);
    spline.points[0].position = {0.0F, 10.0F, 1.0F};
    spline.points[1].position = {2.0F, 20.0F, 3.0F};
    spline.points[2].position = {4.0F, 30.0F, 5.0F};
    spline.points[0].tag = 0;
    spline.points[1].tag = 1;
    spline.points[2].tag = 2;
    spline.payloads.resize(3U);
    spline.payloads[0].camber = 0.5F;
    spline.payloads[1].camber = 0.0F;
    spline.payloads[2].camber = -0.25F;
    const auto primary = apex::app::buildWorkspaceAiSplineGeometry(spline);
    const auto camber =
        apex::app::buildWorkspaceAiSplineCamberGeometry(spline);
    require(primary.ok() && camber.ok(),
            "camber viewport fixtures convert");
    require(camber.geometry.pass ==
                    apex::app::WorkspaceAiSplinePassKind::camber &&
                camber.geometry.mode ==
                    apex::app::WorkspaceAiSplineDisplayMode::raw &&
                camber.geometry.source_point_count == 3U &&
                camber.geometry.sample_point_count == 3U &&
                camber.geometry.vertices.size() == 6U &&
                camber.geometry.chunks.size() == 1U,
            "camber pass retains one line per source point");
    bool saw_green = false;
    bool saw_red = false;
    for (const auto& vertex : camber.geometry.vertices) {
        saw_green = saw_green ||
                    vertex.color ==
                        apex::app::workspace_ai_spline_camber_positive_color;
        saw_red = saw_red ||
                  vertex.color ==
                      apex::app::workspace_ai_spline_camber_nonpositive_color;
    }
    require(saw_green && saw_red,
            "camber pass retains mixed positive and nonpositive colors");

    auto request = request_for(value);
    request.ai_spline_geometry = &primary.geometry;
    request.ai_spline_pipeline = ai_spline_pipeline(value);
    request.ai_spline_camber_geometry = &camber.geometry;
    request.ai_spline_camber_pipeline = ai_spline_camber_pipeline(value);

    auto malformed_primary = primary.geometry;
    malformed_primary.chunks = {{0U, 4U}, {4U, 4U}};
    auto malformed_request = request;
    malformed_request.ai_spline_geometry = &malformed_primary;
    FakeDevice malformed_device;
    const auto malformed_preparation = apex::app::prepareWorkspaceViewport(
        malformed_device, value.document, malformed_request);
    require(!malformed_preparation.ok() &&
                malformed_preparation.diagnostic.code ==
                    "workspace_viewport_ai_spline_chunk_invalid",
            "preparation rejects chunks that overrun the vertex array");

    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "primary and camber passes prepare");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.overlay_counts == std::vector<std::size_t>({2U}) &&
                device.overlay_vertex_counts ==
                    std::vector<std::uint32_t>({4U, 6U}) &&
                device.overlay_depth_tests ==
                    std::vector<bool>({true, true}) &&
                device.overlay_depth_writes ==
                    std::vector<bool>({true, true}) &&
                device.overlay_buffers[0] != device.overlay_buffers[1],
            "camber follows the primary pass with normal depth enabled");

    apex::formats::AiSpline single;
    single.version = 7U;
    single.points.resize(1U);
    single.points[0].position = {1.0F, 2.0F, 3.0F};
    single.points[0].tag = 0;
    single.payloads.resize(1U);
    single.payloads[0].camber = 1.0F;
    const auto single_primary =
        apex::app::buildWorkspaceAiSplineGeometry(single);
    const auto single_camber =
        apex::app::buildWorkspaceAiSplineCamberGeometry(single);
    require(single_primary.ok() && single_camber.ok() &&
                single_camber.geometry.vertices.size() == 2U &&
                single_camber.geometry.chunks.size() == 1U,
            "one-point camber fixture emits one line");
    auto single_request = request_for(value);
    single_request.ai_spline_geometry = &single_primary.geometry;
    single_request.ai_spline_pipeline = ai_spline_pipeline(value);
    single_request.ai_spline_camber_geometry = &single_camber.geometry;
    single_request.ai_spline_camber_pipeline = ai_spline_camber_pipeline(value);
    FakeDevice single_device;
    auto single_prepared = apex::app::prepareWorkspaceViewport(
        single_device, value.document, single_request);
    require(single_prepared.ok(), "one-point camber pass prepares");
    FakeTarget single_target(single_request.presentation);
    Diagnostic single_diagnostic;
    require(single_prepared.viewport->drawAndPresent(
                single_device, single_target, frame, single_diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                single_device.overlay_counts == std::vector<std::size_t>({1U}) &&
                single_device.overlay_vertex_counts ==
                    std::vector<std::uint32_t>({2U}),
            "one-point camber pass submits one immediate line");

    auto missing_primary_request = request_for(value);
    missing_primary_request.ai_spline_camber_geometry = &camber.geometry;
    missing_primary_request.ai_spline_camber_pipeline =
        ai_spline_camber_pipeline(value);
    FakeDevice missing_primary_device;
    const auto missing_primary = apex::app::prepareWorkspaceViewport(
        missing_primary_device, value.document, missing_primary_request);
    require(!missing_primary.ok() &&
                missing_primary.diagnostic.code ==
                    "workspace_viewport_ai_spline_overlay_primary_missing",
            "camber pass cannot be detached from the primary spline");

    auto wrong_pass_geometry = camber.geometry;
    wrong_pass_geometry.pass = apex::app::WorkspaceAiSplinePassKind::primary;
    auto wrong_pass_request = request;
    wrong_pass_request.ai_spline_camber_geometry = &wrong_pass_geometry;
    FakeDevice wrong_pass_device;
    const auto wrong_pass = apex::app::prepareWorkspaceViewport(
        wrong_pass_device, value.document, wrong_pass_request);
    require(!wrong_pass.ok() &&
                wrong_pass.diagnostic.code ==
                    "workspace_viewport_ai_spline_geometry_invalid",
            "camber pass rejects primary metadata");

    auto wrong_color_geometry = camber.geometry;
    wrong_color_geometry.vertices.front().color =
        apex::app::workspace_ai_spline_side_color;
    auto wrong_color_request = request;
    wrong_color_request.ai_spline_camber_geometry = &wrong_color_geometry;
    FakeDevice wrong_color_device;
    const auto wrong_color = apex::app::prepareWorkspaceViewport(
        wrong_color_device, value.document, wrong_color_request);
    require(!wrong_color.ok() &&
                wrong_color.diagnostic.code ==
                    "workspace_viewport_ai_spline_vertex_invalid",
            "camber pass rejects a non-camber color");

    auto nonvertical_geometry = camber.geometry;
    nonvertical_geometry.vertices[1U].position[0] += 1.0F;
    auto nonvertical_request = request;
    nonvertical_request.ai_spline_camber_geometry = &nonvertical_geometry;
    FakeDevice nonvertical_device;
    const auto nonvertical = apex::app::prepareWorkspaceViewport(
        nonvertical_device, value.document, nonvertical_request);
    require(!nonvertical.ok() &&
                nonvertical.diagnostic.code ==
                    "workspace_viewport_ai_spline_camber_line_invalid",
            "camber pass rejects a nonvertical line");

    auto downward_geometry = camber.geometry;
    downward_geometry.vertices[1U].position[1] =
        downward_geometry.vertices[0U].position[1] - 1.0F;
    auto downward_request = request;
    downward_request.ai_spline_camber_geometry = &downward_geometry;
    FakeDevice downward_device;
    const auto downward = apex::app::prepareWorkspaceViewport(
        downward_device, value.document, downward_request);
    require(!downward.ok() &&
                downward.diagnostic.code ==
                    "workspace_viewport_ai_spline_camber_line_invalid",
            "camber pass rejects a downward line");

    auto mismatched_color_geometry = camber.geometry;
    mismatched_color_geometry.vertices[1U].color =
        apex::app::workspace_ai_spline_camber_nonpositive_color;
    auto mismatched_color_request = request;
    mismatched_color_request.ai_spline_camber_geometry =
        &mismatched_color_geometry;
    FakeDevice mismatched_color_device;
    const auto mismatched_color = apex::app::prepareWorkspaceViewport(
        mismatched_color_device, value.document, mismatched_color_request);
    require(!mismatched_color.ok() &&
                mismatched_color.diagnostic.code ==
                    "workspace_viewport_ai_spline_camber_line_invalid",
            "camber pass rejects mismatched endpoint colors");

    apex::formats::AiSpline maximum_spline;
    maximum_spline.version = 7U;
    maximum_spline.points.resize(
        max_overlay_line_total_vertices / 2U + 1U);
    const auto maximum_primary =
        apex::app::buildWorkspaceAiSplineGeometry(maximum_spline);
    require(maximum_primary.ok(), "maximum primary camber fixture converts");
    auto over_budget_request = request_for(value);
    over_budget_request.ai_spline_geometry = &maximum_primary.geometry;
    over_budget_request.ai_spline_pipeline = ai_spline_pipeline(value);
    over_budget_request.ai_spline_camber_geometry = &camber.geometry;
    over_budget_request.ai_spline_camber_pipeline =
        ai_spline_camber_pipeline(value);
    FakeDevice over_budget_device;
    const auto over_budget = apex::app::prepareWorkspaceViewport(
        over_budget_device, value.document, over_budget_request);
    require(!over_budget.ok() &&
                over_budget.diagnostic.code ==
                    "workspace_viewport_ai_spline_limit",
            "camber shares the bounded AI spline aggregate budget");
}

void draws_recovered_ai_spline_selected_index_pass() {
    auto value = fixture();
    apex::formats::AiSpline spline;
    spline.version = 7U;
    spline.points.resize(3U);
    spline.points[0U].position = {0.0F, 0.0F, 0.0F};
    spline.points[1U].position = {10.0F, 0.0F, 0.0F};
    spline.points[2U].position = {20.0F, 0.0F, 0.0F};
    spline.points[0U].tag = 0;
    spline.points[1U].tag = 1;
    spline.points[2U].tag = 2;
    spline.payloads.resize(3U);
    spline.payloads[0U].side0 = 1.0F;
    spline.payloads[0U].side1 = 2.0F;
    const auto primary = apex::app::buildWorkspaceAiSplineGeometry(spline);
    const std::array<std::uint32_t, 2U> selected = {0U, 1U};
    const auto selection =
        apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, selected);
    require(primary.ok() && selection.ok(),
            "selected-index viewport fixtures convert");

    auto request = request_for(value);
    request.ai_spline_geometry = &primary.geometry;
    request.ai_spline_pipeline = ai_spline_pipeline(value);
    request.ai_spline_selection_geometry = &selection.geometry;
    request.ai_spline_selection_pipeline = ai_spline_camber_pipeline(value);
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "primary and current-index passes prepare");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.overlay_counts == std::vector<std::size_t>({2U}) &&
                device.overlay_vertex_counts ==
                    std::vector<std::uint32_t>({4U, 8U}) &&
                device.overlay_depth_tests ==
                    std::vector<bool>({true, true}) &&
                device.overlay_depth_writes ==
                    std::vector<bool>({true, true}),
            "selected-index markers follow the primary pass with normal depth");

    auto missing_primary_request = request_for(value);
    missing_primary_request.ai_spline_selection_geometry =
        &selection.geometry;
    missing_primary_request.ai_spline_selection_pipeline =
        ai_spline_camber_pipeline(value);
    FakeDevice missing_primary_device;
    const auto missing_primary = apex::app::prepareWorkspaceViewport(
        missing_primary_device, value.document, missing_primary_request);
    require(!missing_primary.ok() && missing_primary.diagnostic.code ==
                "workspace_viewport_ai_spline_overlay_primary_missing",
            "selected-index markers cannot be detached from the primary spline");

    auto wrong_color_geometry = selection.geometry;
    wrong_color_geometry.vertices[0U].color =
        apex::app::workspace_ai_spline_selection_side_color;
    auto wrong_color_request = request;
    wrong_color_request.ai_spline_selection_geometry = &wrong_color_geometry;
    FakeDevice wrong_color_device;
    const auto wrong_color = apex::app::prepareWorkspaceViewport(
        wrong_color_device, value.document, wrong_color_request);
    require(!wrong_color.ok() && wrong_color.diagnostic.code ==
                "workspace_viewport_ai_spline_selection_line_invalid",
            "selected-index markers reject reordered recovered colors");

    auto nonvertical_geometry = selection.geometry;
    nonvertical_geometry.vertices[1U].position[0] += 1.0F;
    auto nonvertical_request = request;
    nonvertical_request.ai_spline_selection_geometry =
        &nonvertical_geometry;
    FakeDevice nonvertical_device;
    const auto nonvertical = apex::app::prepareWorkspaceViewport(
        nonvertical_device, value.document, nonvertical_request);
    require(!nonvertical.ok() && nonvertical.diagnostic.code ==
                "workspace_viewport_ai_spline_selection_line_invalid",
            "selected-index markers reject a nonvertical line");

    auto wrong_height_geometry = selection.geometry;
    wrong_height_geometry.vertices[1U].position[1] += 1.0F;
    auto wrong_height_request = request;
    wrong_height_request.ai_spline_selection_geometry =
        &wrong_height_geometry;
    FakeDevice wrong_height_device;
    const auto wrong_height = apex::app::prepareWorkspaceViewport(
        wrong_height_device, value.document, wrong_height_request);
    require(!wrong_height.ok() &&
                wrong_height.diagnostic.code ==
                    "workspace_viewport_ai_spline_selection_line_invalid",
            "selected-index markers reject an incorrect recovered height");

    auto incomplete_side_geometry = selection.geometry;
    incomplete_side_geometry.vertices.erase(
        incomplete_side_geometry.vertices.begin() + 4U,
        incomplete_side_geometry.vertices.begin() + 6U);
    incomplete_side_geometry.sample_point_count = 3U;
    incomplete_side_geometry.chunks[0U].vertex_count = 6U;
    auto incomplete_side_request = request;
    incomplete_side_request.ai_spline_selection_geometry =
        &incomplete_side_geometry;
    FakeDevice incomplete_side_device;
    const auto incomplete_side = apex::app::prepareWorkspaceViewport(
        incomplete_side_device, value.document, incomplete_side_request);
    require(!incomplete_side.ok() &&
                incomplete_side.diagnostic.code ==
                    "workspace_viewport_ai_spline_selection_line_invalid",
            "selected-index markers reject incomplete side-line groups");

    auto wrong_topology_geometry = selection.geometry;
    wrong_topology_geometry.topology =
        apex::app::WorkspaceAiSplineTopology::polyline;
    auto wrong_topology_request = request;
    wrong_topology_request.ai_spline_selection_geometry =
        &wrong_topology_geometry;
    FakeDevice wrong_topology_device;
    const auto wrong_topology = apex::app::prepareWorkspaceViewport(
        wrong_topology_device, value.document, wrong_topology_request);
    require(!wrong_topology.ok() &&
                wrong_topology.diagnostic.code ==
                    "workspace_viewport_ai_spline_geometry_invalid",
            "selected-index markers reject polyline topology");

    auto wrong_state_geometry = selection.geometry;
    wrong_state_geometry.last_selected_index =
        wrong_state_geometry.source_point_count;
    auto wrong_state_request = request;
    wrong_state_request.ai_spline_selection_geometry = &wrong_state_geometry;
    FakeDevice wrong_state_device;
    const auto wrong_state = apex::app::prepareWorkspaceViewport(
        wrong_state_device, value.document, wrong_state_request);
    require(!wrong_state.ok() &&
                wrong_state.diagnostic.code ==
                    "workspace_viewport_ai_spline_geometry_invalid",
            "selected-index markers reject an invalid last-selected index");
}

void replaces_committed_ai_spline_overlays_atomically() {
    auto value = fixture();
    apex::formats::AiSpline spline;
    spline.source = "live-position.ai";
    spline.version = 7U;
    spline.points.resize(4U);
    spline.points[0U].position = {0.0F, 0.0F, 0.0F};
    spline.points[1U].position = {10.0F, 0.0F, 0.0F};
    spline.points[2U].position = {20.0F, 5.0F, 5.0F};
    spline.points[3U].position = {30.0F, 5.0F, 10.0F};
    spline.payloads.resize(spline.points.size());
    for (std::size_t index = 0U; index < spline.points.size(); ++index) {
        spline.points[index].tag = static_cast<std::int32_t>(index);
        spline.payloads[index].side0 = 1.0F;
        spline.payloads[index].side1 = 2.0F;
        spline.payloads[index].camber = index % 2U == 0U ? 0.01F : -0.01F;
    }
    const std::array<std::uint32_t, 2U> selected = {0U, 1U};
    const auto primary = apex::app::buildWorkspaceAiSplineGeometry(spline);
    const auto interval = apex::app::buildWorkspaceAiSplineIntervalGeometry(
        spline, {0.25F, 0.2504F});
    const auto left = apex::app::buildWorkspaceAiSplineSideGeometry(
        spline, apex::app::WorkspaceAiSplineSide::left);
    const auto right = apex::app::buildWorkspaceAiSplineSideGeometry(
        spline, apex::app::WorkspaceAiSplineSide::right);
    const auto selection =
        apex::app::buildWorkspaceAiSplineSelectionGeometry(spline, selected);
    const auto camber = apex::app::buildWorkspaceAiSplineCamberGeometry(spline);
    require(primary.ok() && interval.ok() && left.ok() && right.ok() &&
                selection.ok() && camber.ok(),
            "initial live AI spline passes convert");

    auto request = request_for(value);
    request.ai_spline_geometry = &primary.geometry;
    request.ai_spline_pipeline = ai_spline_pipeline(value);
    request.ai_spline_interval_geometry = &interval.geometry;
    request.ai_spline_interval_pipeline = ai_spline_interval_pipeline(value);
    request.ai_spline_left_geometry = &left.geometry;
    request.ai_spline_left_pipeline = ai_spline_side_pipeline(value);
    request.ai_spline_right_geometry = &right.geometry;
    request.ai_spline_right_pipeline = ai_spline_side_pipeline(value);
    request.ai_spline_selection_geometry = &selection.geometry;
    request.ai_spline_selection_pipeline = ai_spline_camber_pipeline(value);
    request.ai_spline_camber_geometry = &camber.geometry;
    request.ai_spline_camber_pipeline = ai_spline_camber_pipeline(value);
    FakeDevice device;
    auto prepared =
        apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(prepared.ok(), "all live AI spline passes prepare together");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    require(
        prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
            device.overlay_buffers.size() == 6U,
        "initial live AI spline generation draws all passes");
    const std::vector<const Buffer *> initial_buffers = device.overlay_buffers;

    const std::array<std::uint32_t, 1U> invalid_selected = {99U};
    apex::app::WorkspaceAiSplineOverlayRequest invalid_overlay_request;
    invalid_overlay_request.show_left = true;
    invalid_overlay_request.selected_indices = invalid_selected;
    const auto invalid_overlays = apex::app::buildWorkspaceAiSplineOverlays(
        spline, invalid_overlay_request);
    require(!invalid_overlays.ok() &&
                invalid_overlays.diagnostic.code ==
                    "workspace_ai_spline_selection_index_invalid" &&
                invalid_overlays.overlays.primary.vertices.empty() &&
                !invalid_overlays.overlays.left.has_value(),
            "failed multi-pass build publishes no partial overlay generation");

    apex::authoring::AiSplineSession session(spline);
    const std::array<apex::authoring::AiSplinePointPositionEdit, 2U> edits{
        apex::authoring::AiSplinePointPositionEdit{0U, {2.0F, 1.0F, 3.0F}},
        apex::authoring::AiSplinePointPositionEdit{1U, {12.0F, 2.0F, 4.0F}},
    };
    const auto edited = session.setPointPositions(edits);
    apex::app::WorkspaceAiSplineOverlayRequest overlay_request;
    overlay_request.interval =
        apex::app::WorkspaceAiSplineInterval{0.25F, 0.2504F};
    overlay_request.show_left = true;
    overlay_request.show_right = true;
    overlay_request.selected_indices = selected;
    overlay_request.show_camber = true;
    auto updated = apex::app::buildWorkspaceAiSplineOverlays(
        session.current(), overlay_request);
    require(edited.ok() && edited.changed && updated.ok() &&
                updated.overlays.interval.has_value() &&
                updated.overlays.left.has_value() &&
                updated.overlays.right.has_value() &&
                updated.overlays.selection.has_value() &&
                updated.overlays.camber.has_value(),
            "one committed model rebuilds every enabled live pass");

    const auto buffers_before_update = device.buffer_calls;
    const auto replaced =
        prepared.viewport->replaceAiSplineOverlays(device, updated.overlays);
    require(replaced.ok() && replaced.replaced_pass_count == 6U &&
                device.buffer_calls == buffers_before_update + 6U,
            "live update allocates one complete immutable pass generation");

    const std::array<const apex::app::WorkspaceAiSplineGeometry *, 6U>
        updated_geometries = {
            &updated.overlays.primary,    &*updated.overlays.interval,
            &*updated.overlays.left,      &*updated.overlays.right,
            &*updated.overlays.selection, &*updated.overlays.camber};
    std::array<std::vector<std::byte>, 6U> updated_bytes;
    for (std::size_t index = 0U; index < updated_geometries.size(); ++index) {
        const auto bytes =
            std::as_bytes(std::span(updated_geometries[index]->vertices));
        updated_bytes[index] = {bytes.begin(), bytes.end()};
    }
    updated.overlays = {};
    require(
        prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
            device.overlay_buffers.size() == 12U,
        "next frame draws the committed live pass generation");
    const std::vector<const Buffer *> committed_buffers(
        device.overlay_buffers.end() - 6, device.overlay_buffers.end());
    require(std::equal(initial_buffers.begin(), initial_buffers.end(),
                       committed_buffers.begin(),
                       [](const Buffer *before, const Buffer *after) {
                           return before != after;
                       }),
            "live update replaces every retained AI spline buffer");
    for (std::size_t index = 0U; index < committed_buffers.size(); ++index) {
        const auto *buffer =
            dynamic_cast<const FakeBuffer *>(committed_buffers[index]);
        require(buffer != nullptr && buffer->bytes() == updated_bytes[index],
                "live AI spline buffer owns bytes after input release");
    }

    const std::array<apex::authoring::AiSplinePointPositionEdit, 1U>
        next_edits{apex::authoring::AiSplinePointPositionEdit{
            2U, {22.0F, 7.0F, 8.0F}}};
    const auto next_edited = session.setPointPositions(next_edits);
    auto next = apex::app::buildWorkspaceAiSplineOverlays(session.current(),
                                                           overlay_request);
    require(next_edited.ok() && next_edited.changed && next.ok(),
            "a newer model generation rebuilds every live pass");

    auto malformed = next.overlays;
    malformed.primary.chunks = {{0U, 4U}, {4U, 4U}};
    const auto buffers_before_malformed = device.buffer_calls;
    const auto malformed_update =
        prepared.viewport->replaceAiSplineOverlays(device, malformed);
    require(!malformed_update.ok() &&
                malformed_update.diagnostic.code ==
                    "workspace_viewport_ai_spline_chunk_invalid" &&
                device.buffer_calls == buffers_before_malformed,
            "live replacement rejects overrun chunks before allocation");

    auto over_limit = next.overlays;
    over_limit.primary.vertices.resize(
        apex::render::max_overlay_line_total_vertices + 1U);
    const auto over_limit_update =
        prepared.viewport->replaceAiSplineOverlays(device, over_limit);
    require(!over_limit_update.ok() &&
                over_limit_update.diagnostic.code ==
                    "workspace_viewport_ai_spline_limit" &&
                device.buffer_calls == buffers_before_malformed,
            "live replacement checks aggregate limits before allocation");

    auto invalid = next.overlays;
    invalid.primary.vertices.front().position[0] =
        std::numeric_limits<float>::infinity();
    const auto rejected =
        prepared.viewport->replaceAiSplineOverlays(device, invalid);
    require(!rejected.ok() && rejected.diagnostic.code ==
                                  "workspace_viewport_ai_spline_vertex_invalid",
            "non-finite live replacement is rejected before allocation");

    const auto live_buffers_before_failure = *device.live_buffer_count;
    device.fail_buffer_call = device.buffer_calls + 3U;
    device.fail_buffer_status = BufferStatus::upload_failed;
    const auto upload_failed =
        prepared.viewport->replaceAiSplineOverlays(device, next.overlays);
    require(!upload_failed.ok() &&
                upload_failed.status ==
                    apex::app::WorkspaceViewportAiSplineUpdateStatus::
                        upload_failed &&
                upload_failed.diagnostic.code == "fake_buffer_failed",
            "mid-generation upload failure reports its exact status");
    require(*device.live_buffer_count == live_buffers_before_failure,
            "mid-generation failure releases only temporary buffers");
    device.fail_buffer_call = 0U;

    require(
        prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
            WorkspaceViewportFrameStatus::ready,
        "failed live replacements keep the viewport drawable");
    const auto failed_frame_begin = device.overlay_buffers.end() - 6;
    require(std::equal(committed_buffers.begin(), committed_buffers.end(),
                       failed_frame_begin),
            "newer-model upload failure retains the prior generation");

    auto missing_pass = next.overlays;
    missing_pass.camber.reset();
    const auto mismatched =
        prepared.viewport->replaceAiSplineOverlays(device, missing_pass);
    require(!mismatched.ok() &&
                mismatched.diagnostic.code ==
                    "workspace_viewport_ai_spline_update_configuration_invalid",
            "live replacement cannot detach a prepared pass");
    FakeDevice foreign_device;
    const auto foreign =
        prepared.viewport->replaceAiSplineOverlays(foreign_device,
                                                   next.overlays);
    require(!foreign.ok() &&
                foreign.diagnostic.code ==
                    "workspace_viewport_ai_spline_update_device_mismatch" &&
                foreign_device.buffer_calls == 0U,
            "live replacement rejects a different same-backend device");
}

void tracks_recovered_ai_spline_manual_input() {
    using apex::app::WorkspaceAiSplineManualInputState;
    using apex::app::WorkspaceAiSplineManualKey;

    WorkspaceAiSplineManualInputState input;
    require(input.setPressed(WorkspaceAiSplineManualKey::forward, true) &&
                !input.setPressed(WorkspaceAiSplineManualKey::forward, true),
            "repeated manual key-down is idempotent");
    require(input.setPressed(WorkspaceAiSplineManualKey::right, true) &&
                input.setPressed(WorkspaceAiSplineManualKey::up, true),
            "independent manual direction keys combine");
    const auto normal =
        apex::app::workspaceAiSplineManualLocalDelta(input.movement());
    const float normalAmount =
        apex::app::workspace_ai_spline_manual_speed *
        apex::app::workspace_ai_spline_manual_fixed_delta;
    require(std::abs(normal[0U] - normalAmount) < 1.0e-7F &&
                std::abs(normal[1U] - normalAmount) < 1.0e-7F &&
                std::abs(normal[2U] + normalAmount) < 1.0e-7F,
            "manual keys use the recovered local signs and fixed delta");

    require(input.setPressed(WorkspaceAiSplineManualKey::left_control, true) &&
                input.setPressed(WorkspaceAiSplineManualKey::right_control,
                                 true),
            "left and right Control have independent held state");
    require(input.setPressed(WorkspaceAiSplineManualKey::left_control, false) &&
                input.movement().accelerated,
            "releasing one Control retains acceleration from the other");
    const auto accelerated =
        apex::app::workspaceAiSplineManualLocalDelta(input.movement());
    require(std::abs(accelerated[0U] - normal[0U] * 10.0F) < 1.0e-6F &&
                std::abs(accelerated[1U] - normal[1U] * 10.0F) < 1.0e-6F &&
                std::abs(accelerated[2U] - normal[2U] * 10.0F) < 1.0e-6F,
            "Control applies the recovered ten-times movement scale");

    require(input.setPressed(WorkspaceAiSplineManualKey::backward, true) &&
                input.setPressed(WorkspaceAiSplineManualKey::left, true) &&
                input.setPressed(WorkspaceAiSplineManualKey::down, true),
            "opposite manual directions can be held together");
    const auto cancelled =
        apex::app::workspaceAiSplineManualLocalDelta(input.movement());
    require(cancelled == std::array<float, 3U>{0.0F, 0.0F, 0.0F},
            "opposite manual directions cancel exactly");
    input.clear();
    require(input.movement() == apex::app::WorkspaceAiSplineManualMovement{},
            "focus-loss clear releases every manual input bit");
    input.setFocused(false);
    require(!input.setPressed(WorkspaceAiSplineManualKey::forward, true) &&
                input.movement() ==
                    apex::app::WorkspaceAiSplineManualMovement{},
            "unfocused manual input ignores new key-down events");
    input.setFocused(true);
    require(input.setPressed(WorkspaceAiSplineManualKey::forward, true),
            "manual input resumes after explicit focus gain");
    input.clear();
    require(!input.setPressed(
                static_cast<WorkspaceAiSplineManualKey>(255U), true),
            "unknown manual key values do not change input state");
}

void routes_portable_ai_spline_side_visibility_commands() {
    apex::platform::WindowEvent event;
    event.type = apex::platform::WindowEventType::key_down;
    event.semantic_key = apex::platform::WindowKey::l;
    event.modifiers = static_cast<std::uint32_t>(
        apex::platform::WindowModifier::control);
    require(apex::app::workspaceAiSplineSideVisibilityCommand(event) ==
                apex::app::WorkspaceAiSplineSideVisibilityCommand::
                    toggle_left,
            "Control+L routes to the portable left-side action");

    event.semantic_key = apex::platform::WindowKey::r;
    event.modifiers |= static_cast<std::uint32_t>(
        apex::platform::WindowModifier::shift);
    require(apex::app::workspaceAiSplineSideVisibilityCommand(event) ==
                apex::app::WorkspaceAiSplineSideVisibilityCommand::
                    toggle_right,
            "Control+R routes to the portable right-side action");

    event.repeat = true;
    require(!apex::app::workspaceAiSplineSideVisibilityCommand(event)
                 .has_value(),
            "repeated visibility keys do not produce commands");
    event.repeat = false;
    event.modifiers = 0U;
    require(!apex::app::workspaceAiSplineSideVisibilityCommand(event)
                 .has_value(),
            "visibility keys require the Control modifier");
    event.modifiers = static_cast<std::uint32_t>(
        apex::platform::WindowModifier::control);
    event.type = apex::platform::WindowEventType::key_up;
    require(!apex::app::workspaceAiSplineSideVisibilityCommand(event)
                 .has_value(),
            "visibility commands use key-down events only");
    event.type = apex::platform::WindowEventType::key_down;
    event.semantic_key = static_cast<apex::platform::WindowKey>(255U);
    require(!apex::app::workspaceAiSplineSideVisibilityCommand(event)
                 .has_value(),
            "unknown semantic keys do not produce visibility commands");
}

void publishes_ai_spline_side_visibility_atomically() {
    apex::formats::AiSpline spline;
    spline.source = "controller-side-visibility.ai";
    spline.version = 7U;
    spline.points.resize(4U);
    spline.payloads.resize(4U);
    for (std::size_t index = 0U; index < spline.points.size(); ++index) {
        spline.points[index].position = {
            static_cast<float>(index) * 10.0F, 0.0F,
            static_cast<float>(index)};
        spline.points[index].tag = static_cast<std::int32_t>(index);
        spline.payloads[index].side0 = 1.0F;
        spline.payloads[index].side1 = 2.0F;
    }
    apex::app::WorkspaceAiSplineControllerConfiguration configuration;
    configuration.selectedIndices = {0U};
    auto created = apex::app::WorkspaceAiSplineController::create(
        spline, std::move(configuration));
    require(created.ok(), "side-visibility controller creates");
    auto controller = std::move(created.controller);

    auto value = fixture();
    auto request = request_for(value);
    request.ai_spline_geometry = &controller->overlays().primary;
    request.ai_spline_generation = controller->generation();
    request.ai_spline_pipeline = ai_spline_pipeline(value);
    request.ai_spline_left_pipeline = ai_spline_side_pipeline(value);
    request.ai_spline_right_pipeline = ai_spline_side_pipeline(value);
    request.ai_spline_selection_geometry =
        &*controller->overlays().selection;
    request.ai_spline_selection_pipeline = ai_spline_camber_pipeline(value);
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(),
            "hidden sides prepare latent independent side pipelines");

    const auto baselineBytes = controller->currentBytes();
    const auto baselineInput = controller->inputSnapshot();
    const auto baselineBuffers = *device.live_buffer_count;
    auto staleInput = baselineInput;
    ++staleInput.inputEpoch;
    const auto stale = controller->setSideVisibility(
        device, *prepared.viewport, true, false, staleInput);
    require(!stale.ok() &&
                stale.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::stale_input &&
                controller->inputSnapshot() == baselineInput &&
                *device.live_buffer_count == baselineBuffers,
            "side visibility rejects a stale input before allocation");

    const auto unchanged = controller->setSideVisibility(
        device, *prepared.viewport, false, false,
        controller->inputSnapshot());
    require(unchanged.ok() && !unchanged.changed &&
                unchanged.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::unchanged &&
                controller->inputSnapshot() == baselineInput,
            "unchanged side visibility performs no publication");

    const auto leftVisible = controller->setSideVisibility(
        device, *prepared.viewport, true, false,
        controller->inputSnapshot());
    require(leftVisible.ok() && leftVisible.changed &&
                leftVisible.replacedPassCount == 4U &&
                controller->configuration().showLeft &&
                !controller->configuration().showRight &&
                controller->overlays().left.has_value() &&
                !controller->overlays().right.has_value() &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>{0U} &&
                controller->revision() == 0U && !controller->dirty() &&
                !controller->canUndo() && !controller->canRedo() &&
                controller->currentBytes() == baselineBytes &&
                controller->inputSnapshot().inputEpoch ==
                    baselineInput.inputEpoch + 1U &&
                controller->generation().publication ==
                    baselineInput.generation.publication + 1U &&
                prepared.viewport->aiSplineGenerationIdentity() ==
                    controller->generation(),
            "left-side visibility publishes without changing authoring state");

    const auto beforeFailure = controller->inputSnapshot();
    const auto liveBeforeFailure = *device.live_buffer_count;
    device.fail_buffer_call = device.buffer_calls + 3U;
    device.fail_buffer_status = BufferStatus::upload_failed;
    const auto failed = controller->setSideVisibility(
        device, *prepared.viewport, true, true,
        controller->inputSnapshot());
    require(!failed.ok() &&
                failed.status == apex::app::WorkspaceAiSplineControllerStatus::
                                     viewport_failed &&
                controller->configuration().showLeft &&
                !controller->configuration().showRight &&
                controller->inputSnapshot() == beforeFailure &&
                controller->currentBytes() == baselineBytes &&
                *device.live_buffer_count == liveBeforeFailure &&
                prepared.viewport->aiSplineGenerationIdentity() ==
                    controller->generation(),
            "side visibility upload failure preserves controller and viewport state");
    device.fail_buffer_call = 0U;

    const auto bothVisible = controller->setSideVisibility(
        device, *prepared.viewport, true, true,
        controller->inputSnapshot());
    require(bothVisible.ok() && bothVisible.changed &&
                controller->overlays().left.has_value() &&
                controller->overlays().right.has_value(),
            "right-side visibility publishes independently after retry");
    const auto started = controller->startEditing(
        device, *prepared.viewport, controller->inputSnapshot());
    require(started.ok() && started.changed && controller->editing(),
            "side-visibility controller enters edit mode");
    const auto hidden = controller->setSideVisibility(
        device, *prepared.viewport, false, false,
        controller->inputSnapshot());
    require(hidden.ok() && hidden.changed && controller->editing() &&
                !controller->overlays().left.has_value() &&
                !controller->overlays().right.has_value() &&
                controller->revision() == 0U &&
                controller->currentBytes() == baselineBytes,
            "side visibility preserves the active edit mode");
}

void publishes_ai_spline_controller_transactions() {
    auto value = fixture();
    apex::formats::AiSpline spline;
    spline.source = "controller-position.ai";
    spline.version = 7U;
    spline.points.resize(4U);
    spline.points[0U].position = {0.0F, 0.0F, 0.0F};
    spline.points[1U].position = {10.0F, 0.0F, 0.0F};
    spline.points[2U].position = {20.0F, 5.0F, 5.0F};
    spline.points[3U].position = {30.0F, 5.0F, 10.0F};
    spline.payloads.resize(spline.points.size());
    for (std::size_t index = 0U; index < spline.points.size(); ++index) {
        spline.points[index].tag = static_cast<std::int32_t>(index);
        spline.payloads[index].side0 = 1.0F;
        spline.payloads[index].side1 = 2.0F;
        spline.payloads[index].camber =
            index % 2U == 0U ? 0.01F : -0.01F;
    }
    apex::app::WorkspaceAiSplineControllerConfiguration configuration;
    configuration.interval =
        apex::app::WorkspaceAiSplineInterval{0.25F, 0.2504F};
    configuration.showLeft = true;
    configuration.showRight = true;
    configuration.selectedIndices = {0U, 1U, 0U, 1U};
    configuration.showCamber = true;
    auto created = apex::app::WorkspaceAiSplineController::create(
        std::move(spline), std::move(configuration));
    require(created.ok() && created.controller->revision() == 0U &&
                !created.controller->dirty() &&
                created.controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({0U, 1U}),
            "AI spline controller owns one canonical clean generation");
    auto controller = std::move(created.controller);
    const auto baselineBytes = controller->currentBytes();

    const auto& overlays = controller->overlays();
    auto request = request_for(value);
    request.ai_spline_geometry = &overlays.primary;
    request.ai_spline_generation = controller->generation();
    request.ai_spline_pipeline = ai_spline_pipeline(value);
    request.ai_spline_interval_geometry = &*overlays.interval;
    request.ai_spline_interval_pipeline = ai_spline_interval_pipeline(value);
    request.ai_spline_left_geometry = &*overlays.left;
    request.ai_spline_left_pipeline = ai_spline_side_pipeline(value);
    request.ai_spline_right_geometry = &*overlays.right;
    request.ai_spline_right_pipeline = ai_spline_side_pipeline(value);
    request.ai_spline_selection_geometry = &*overlays.selection;
    request.ai_spline_selection_pipeline = ai_spline_camber_pipeline(value);
    request.ai_spline_camber_geometry = &*overlays.camber;
    request.ai_spline_camber_pipeline = ai_spline_camber_pipeline(value);
    FakeDevice device;
    auto prepared =
        apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(prepared.ok(), "controller AI spline viewport prepares");
    auto unboundRequest = request;
    unboundRequest.ai_spline_generation.reset();
    auto unbound = apex::app::prepareWorkspaceViewport(
        device, value.document, unboundRequest);
    require(unbound.ok(), "untracked AI spline viewport prepares");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    auto drawControllerGeneration = [&]() {
        const auto previousCount = device.overlay_buffers.size();
        require(prepared.viewport->drawAndPresent(device, target, frame,
                                                  diagnostic) ==
                        WorkspaceViewportFrameStatus::ready &&
                    device.overlay_buffers.size() == previousCount + 6U,
                "controller generation draws all prepared passes");
        std::vector<const Buffer*> buffers(device.overlay_buffers.end() - 6,
                                           device.overlay_buffers.end());
        const auto* primary = dynamic_cast<const FakeBuffer*>(buffers.front());
        const auto expected = std::as_bytes(
            std::span(controller->overlays().primary.vertices));
        require(primary != nullptr &&
                    primary->bytes() ==
                        std::vector<std::byte>(expected.begin(), expected.end()),
                "drawn primary bytes match the controller generation");
        return buffers;
    };
    const auto baselineBuffers = drawControllerGeneration();
    const auto initialBufferCalls = device.buffer_calls;

    const std::array<apex::authoring::AiSplinePointPositionEdit, 1U> noOp{
        apex::authoring::AiSplinePointPositionEdit{0U, {0.0F, 0.0F, 0.0F}}};
    const auto callsBeforeBindingCheck = device.buffer_calls;
    const auto unboundEdit = controller->setPointPositions(
        device, *unbound.viewport, noOp, controller->revision());
    require(!unboundEdit.ok() &&
                unboundEdit.diagnostic.code ==
                    "workspace_ai_spline_controller_viewport_generation_mismatch" &&
                device.buffer_calls == callsBeforeBindingCheck,
            "controller rejects a viewport without its visible revision");
    const auto unchanged = controller->setPointPositions(
        device, *prepared.viewport, noOp, controller->revision());
    require(unchanged.ok() && !unchanged.changed &&
                unchanged.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::unchanged &&
                controller->revision() == 0U && !controller->dirty() &&
                device.buffer_calls == initialBufferCalls,
            "no-op edit changes no session or viewport state");

    const std::array<apex::authoring::AiSplinePointPositionEdit, 1U> edit{
        apex::authoring::AiSplinePointPositionEdit{0U, {2.0F, 1.0F, 3.0F}}};
    const auto stale = controller->setPointPositions(
        device, *prepared.viewport, edit, 99U);
    require(!stale.ok() &&
                stale.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        stale_revision &&
                stale.diagnostic.code ==
                    "workspace_ai_spline_controller_revision_stale" &&
                device.buffer_calls == initialBufferCalls,
            "stale edit revision fails before candidate allocation");

    const auto changed = controller->setPointPositions(
        device, *prepared.viewport, edit, 0U);
    require(changed.ok() && changed.changed && changed.revision == 1U &&
                changed.replacedPassCount == 6U &&
                controller->revision() == 1U && controller->dirty() &&
                prepared.viewport->aiSplineRevision() == 1U &&
                controller->current().points[0U].position == edit[0U].position,
            "controller commits the model only after all passes upload");
    require(
        prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
            device.overlay_buffers.size() == 12U,
        "next frame draws the controller's edited generation");
    const std::vector<const Buffer*> editedBuffers(
        device.overlay_buffers.end() - 6, device.overlay_buffers.end());
    require(std::equal(baselineBuffers.begin(), baselineBuffers.end(),
                       editedBuffers.begin(),
                       [](const Buffer* before, const Buffer* after) {
                           return before != after;
                       }),
            "controller edit replaces every prepared pass");

    const std::array<apex::authoring::AiSplinePointPositionEdit, 1U> nextEdit{
        apex::authoring::AiSplinePointPositionEdit{1U, {12.0F, 2.0F, 4.0F}}};
    const auto liveBeforeFailure = *device.live_buffer_count;
    device.fail_buffer_call = device.buffer_calls + 3U;
    device.fail_buffer_status = BufferStatus::upload_failed;
    const auto failed = controller->setPointPositions(
        device, *prepared.viewport, nextEdit, 1U);
    require(!failed.ok() &&
                failed.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        viewport_failed &&
                failed.diagnostic.code == "fake_buffer_failed" &&
                controller->revision() == 1U && controller->dirty() &&
                controller->current().points[1U].position ==
                    std::array<float, 3U>{10.0F, 0.0F, 0.0F} &&
                *device.live_buffer_count == liveBeforeFailure,
            "failed upload leaves model and visible generation at revision one");
    device.fail_buffer_call = 0U;
    require(
        prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready,
        "viewport remains drawable after controller publication failure");
    require(std::equal(editedBuffers.begin(), editedBuffers.end(),
                       device.overlay_buffers.end() - 6),
            "failed controller publication retains all edited buffers");

    device.fail_buffer_call = device.buffer_calls + 1U;
    device.fail_buffer_status = BufferStatus::allocation_failed;
    const auto allocationFailed = controller->setPointPositions(
        device, *prepared.viewport, nextEdit, 1U);
    require(!allocationFailed.ok() &&
                allocationFailed.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        allocation_failed &&
                controller->revision() == 1U,
            "controller preserves an exact allocation-failure status");
    device.fail_buffer_call = 0U;

    const std::array<apex::authoring::AiSplinePointPositionEdit, 2U> conflict{
        apex::authoring::AiSplinePointPositionEdit{1U, {1.0F, 2.0F, 3.0F}},
        apex::authoring::AiSplinePointPositionEdit{1U, {4.0F, 5.0F, 6.0F}},
    };
    const auto callsBeforeConflict = device.buffer_calls;
    const auto conflicted = controller->setPointPositions(
        device, *prepared.viewport, conflict, 1U);
    require(!conflicted.ok() &&
                conflicted.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        invalid_edit &&
                conflicted.diagnostic.code == "POINT_EDIT_CONFLICT" &&
                controller->revision() == 1U &&
                device.buffer_calls == callsBeforeConflict,
            "invalid candidate fails before overlay allocation");

    const auto retried = controller->setPointPositions(
        device, *prepared.viewport, nextEdit, controller->revision());
    require(retried.ok() && retried.changed && retried.revision == 2U &&
                controller->current().points[1U].position ==
                    nextEdit[0U].position,
            "controller rebuilds and publishes a discarded candidate on retry");
    const auto retryBuffers = drawControllerGeneration();
    const auto returnedToEdited =
        controller->undo(device, *prepared.viewport, controller->revision());
    require(returnedToEdited.ok() && returnedToEdited.revision == 3U &&
                controller->dirty() &&
                controller->current().points[0U].position == edit[0U].position &&
                controller->current().points[1U].position ==
                    std::array<float, 3U>{10.0F, 0.0F, 0.0F},
            "first undo returns from the retried candidate to revision B");
    const auto returnedBuffers = drawControllerGeneration();
    require(std::equal(retryBuffers.begin(), retryBuffers.end(),
                       returnedBuffers.begin(),
                       [](const Buffer* before, const Buffer* after) {
                           return before != after;
                       }),
            "next frame draws the successful retry undo");

    const auto undone =
        controller->undo(device, *prepared.viewport, controller->revision());
    require(undone.ok() && undone.changed && undone.revision == 4U &&
                !controller->dirty() && controller->canRedo() &&
                prepared.viewport->aiSplineRevision() == 4U &&
                controller->current().points[0U].position ==
                    std::array<float, 3U>{0.0F, 0.0F, 0.0F},
            "transactional undo publishes baseline and clears dirty state");
    const auto undoBuffers = drawControllerGeneration();
    require(std::equal(returnedBuffers.begin(), returnedBuffers.end(),
                       undoBuffers.begin(),
                       [](const Buffer* before, const Buffer* after) {
                           return before != after;
                       }),
            "next frame draws the undo generation");
    const auto redone =
        controller->redo(device, *prepared.viewport, controller->revision());
    require(redone.ok() && redone.changed && redone.revision == 5U &&
                controller->dirty() &&
                prepared.viewport->aiSplineRevision() == 5U &&
                controller->current().points[0U].position == edit[0U].position,
            "transactional redo republishes the edited generation");
    const auto redoBuffers = drawControllerGeneration();
    require(std::equal(undoBuffers.begin(), undoBuffers.end(),
                       redoBuffers.begin(),
                       [](const Buffer* before, const Buffer* after) {
                           return before != after;
                       }),
            "next frame draws the redo generation");
    const auto restored = controller->restoreBaseline(
        device, *prepared.viewport, controller->revision());
    require(restored.ok() && restored.changed && restored.revision == 6U &&
                !controller->dirty() &&
                prepared.viewport->aiSplineRevision() == 6U &&
                controller->currentBytes() == baselineBytes,
            "transactional reset publishes the baseline and clears dirty");
    const auto resetBuffers = drawControllerGeneration();
    require(std::equal(redoBuffers.begin(), redoBuffers.end(),
                       resetBuffers.begin(),
                       [](const Buffer* before, const Buffer* after) {
                           return before != after;
                       }),
            "next frame draws the reset generation");

    apex::app::WorkspaceAiSplineManualInputState manualInput;
    (void)manualInput.setPressed(
        apex::app::WorkspaceAiSplineManualKey::forward, true);
    (void)manualInput.setPressed(
        apex::app::WorkspaceAiSplineManualKey::right, true);
    (void)manualInput.setPressed(apex::app::WorkspaceAiSplineManualKey::up,
                                 true);
    const auto callsBeforeManual = device.buffer_calls;
    auto staleManualInput = controller->inputSnapshot();
    ++staleManualInput.inputEpoch;
    const auto staleManual = controller->moveSelectedByManualInput(
        device, *prepared.viewport, manualInput.movement(), staleManualInput);
    require(!staleManual.ok() &&
                staleManual.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        stale_input &&
                device.buffer_calls == callsBeforeManual,
            "manual movement rejects a stale input snapshot");
    const auto moved = controller->moveSelectedByManualInput(
        device, *prepared.viewport, manualInput.movement(),
        controller->inputSnapshot());
    const float amount = apex::app::workspace_ai_spline_manual_speed *
                         apex::app::workspace_ai_spline_manual_fixed_delta;
    const float diagonalLength = std::sqrt(125.0F);
    const float headingX = 10.0F / diagonalLength;
    const float headingZ = 5.0F / diagonalLength;
    require(moved.ok() && moved.changed && moved.revision == 7U &&
                moved.replacedPassCount == 6U &&
                device.buffer_calls == callsBeforeManual + 6U &&
                std::abs(controller->current().points[0U].position[0U] -
                         amount) < 1.0e-6F &&
                std::abs(controller->current().points[0U].position[1U] -
                         amount) < 1.0e-6F &&
                std::abs(controller->current().points[0U].position[2U] -
                         amount) < 1.0e-6F &&
                std::abs(controller->current().points[1U].position[0U] -
                         (10.0F + (headingX - headingZ) * amount)) <
                    1.0e-6F &&
                std::abs(controller->current().points[1U].position[1U] -
                         amount) < 1.0e-6F &&
                std::abs(controller->current().points[1U].position[2U] -
                         (headingX + headingZ) * amount) < 1.0e-6F,
            "manual movement publishes one recovered multi-point transform");
    (void)drawControllerGeneration();

    manualInput.clear();
    (void)manualInput.setPressed(
        apex::app::WorkspaceAiSplineManualKey::forward, true);
    const auto pointOneBeforeRetry =
        controller->current().points[1U].position;
    device.fail_buffer_call = device.buffer_calls + 3U;
    device.fail_buffer_status = BufferStatus::upload_failed;
    const auto failedManual = controller->moveSelectedByManualInput(
        device, *prepared.viewport, manualInput.movement(),
        controller->inputSnapshot());
    require(!failedManual.ok() && controller->revision() == 7U &&
                controller->current().points[1U].position ==
                    pointOneBeforeRetry,
            "failed manual publication keeps the prior model revision");
    device.fail_buffer_call = 0U;
    const auto retriedManual = controller->moveSelectedByManualInput(
        device, *prepared.viewport, manualInput.movement(),
        controller->inputSnapshot());
    require(retriedManual.ok() && retriedManual.revision == 8U &&
                std::abs(controller->current().points[1U].position[0U] -
                         (pointOneBeforeRetry[0U] + headingX * amount)) <
                    1.0e-6F &&
                std::abs(controller->current().points[1U].position[2U] -
                         (pointOneBeforeRetry[2U] + headingZ * amount)) <
                    1.0e-6F,
            "manual retry uses one cached-forward step after upload recovery");
    (void)drawControllerGeneration();

    manualInput.clear();
    const auto callsBeforeIdle = device.buffer_calls;
    const auto idle = controller->moveSelectedByManualInput(
        device, *prepared.viewport, manualInput.movement(),
        controller->inputSnapshot());
    require(idle.ok() && !idle.changed && idle.revision == 8U &&
                device.buffer_calls == callsBeforeIdle,
            "released manual input performs no session or buffer work");

    const auto beforeSave = controller->currentBytes();
    const auto beforeSaveRevision = controller->revision();
    const bool beforeSaveDirty = controller->dirty();
    const bool beforeSaveUndo = controller->canUndo();
    const bool beforeSaveRedo = controller->canRedo();
    const auto staleSave = controller->buildSaveBytes(99U);
    require(!staleSave.ok() && staleSave.bytes.empty() &&
                staleSave.status ==
                    apex::app::WorkspaceAiSplineControllerSaveStatus::
                        stale_revision &&
                staleSave.revision == beforeSaveRevision,
            "controller save rejects a stale revision without bytes");
    const auto saved = controller->buildSaveBytes(beforeSaveRevision);
    const auto parsedSaved = apex::formats::parseAiSpline(
        saved.bytes, "controller-save-roundtrip.ai");
    require(saved.ok() && saved.revision == beforeSaveRevision &&
                parsedSaved.grid.has_value() &&
                controller->currentBytes() == beforeSave &&
                controller->revision() == beforeSaveRevision &&
                controller->dirty() == beforeSaveDirty &&
                controller->canUndo() == beforeSaveUndo &&
                controller->canRedo() == beforeSaveRedo &&
                device.buffer_calls == callsBeforeIdle,
            "controller save bytes preserve visible and authoring state");

    auto limitedSpline = controller->current();
    apex::authoring::AiSplineSessionLimits saveLimits;
    saveLimits.grid.maxGridRows = 0U;
    auto limitedController = apex::app::WorkspaceAiSplineController::create(
        std::move(limitedSpline), {}, saveLimits);
    require(limitedController.ok(), "save-limited controller creates");
    const auto limitedSave =
        limitedController.controller->buildSaveBytes(0U);
    require(!limitedSave.ok() && limitedSave.bytes.empty() &&
                limitedSave.status ==
                    apex::app::WorkspaceAiSplineControllerSaveStatus::
                        resource_limit &&
                limitedSave.diagnostic.code == "COUNT_LIMIT",
            "controller reports save resource limits without string matching");
}

void publishes_recovered_ai_spline_edit_lifecycle() {
    apex::formats::AiSpline spline;
    spline.source = "controller-lifecycle.ai";
    spline.version = 7U;
    spline.points.resize(4U);
    spline.payloads.resize(4U);
    for (std::size_t index = 0U; index < spline.points.size(); ++index) {
        spline.points[index].position = {
            static_cast<float>(index) * 10.0F, 0.0F,
            static_cast<float>(index)};
        spline.points[index].tag = static_cast<std::int32_t>(index);
        spline.payloads[index].side0 = 1.0F;
        spline.payloads[index].side1 = 2.0F;
    }

    const auto createController = [&]() {
        apex::app::WorkspaceAiSplineControllerConfiguration configuration;
        configuration.selectedIndices = {0U, 1U};
        return apex::app::WorkspaceAiSplineController::create(
            spline, std::move(configuration));
    };
    const auto prepareController = [&](FakeDevice& device,
                                       apex::app::WorkspaceAiSplineController&
                                           controller) {
        auto value = fixture();
        auto request = request_for(value);
        request.ai_spline_geometry = &controller.overlays().primary;
        request.ai_spline_generation = controller.generation();
        request.ai_spline_pipeline = ai_spline_pipeline(value);
        request.ai_spline_selection_geometry =
            &*controller.overlays().selection;
        request.ai_spline_selection_pipeline =
            ai_spline_camber_pipeline(value);
        return apex::app::prepareWorkspaceViewport(device, value.document,
                                                    request);
    };

    auto created = createController();
    require(created.ok() && !created.controller->editing(),
            "AI spline lifecycle controller starts outside edit mode");
    auto controller = std::move(created.controller);
    require(controller->inputSnapshot().inputEpoch == 0U,
            "AI spline lifecycle input epoch starts at zero");
    FakeDevice device;
    auto prepared = prepareController(device, *controller);
    require(prepared.ok(), "AI spline lifecycle viewport prepares");

    const auto baselineBytes = controller->currentBytes();
    const auto baselineRevision = controller->revision();
    const auto initialInput = controller->inputSnapshot();
    const auto baselineLiveBuffers = *device.live_buffer_count;
    const auto baselineBufferCalls = device.buffer_calls;
    const auto finishBeforeStart = controller->finishEditing(
        device, *prepared.viewport, controller->inputSnapshot());
    require(finishBeforeStart.ok() && !finishBeforeStart.changed &&
                !controller->editing() &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({0U, 1U}) &&
                controller->inputSnapshot().inputEpoch == 0U &&
                device.buffer_calls == baselineBufferCalls,
            "finish preserves selection outside edit mode");

    auto staleInput = controller->inputSnapshot();
    staleInput.generation.revision = 99U;
    const auto stale = controller->startEditing(
        device, *prepared.viewport, staleInput);
    require(!stale.ok() &&
                stale.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        stale_input &&
                !controller->editing() &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({0U, 1U}) &&
                controller->inputSnapshot().inputEpoch == 0U &&
                device.buffer_calls == baselineBufferCalls,
            "start rejects a stale revision before lifecycle allocation");

    device.fail_buffer_call = device.buffer_calls + 1U;
    device.fail_buffer_status = BufferStatus::upload_failed;
    const auto uploadFailed = controller->startEditing(
        device, *prepared.viewport, controller->inputSnapshot());
    require(!uploadFailed.ok() &&
                uploadFailed.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        viewport_failed &&
                !controller->editing() &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({0U, 1U}) &&
                controller->currentBytes() == baselineBytes &&
                controller->revision() == baselineRevision &&
                controller->inputSnapshot().inputEpoch == 0U &&
                prepared.viewport->aiSplineRevision() == baselineRevision &&
                *device.live_buffer_count == baselineLiveBuffers,
            "failed upload keeps lifecycle and viewport state");
    device.fail_buffer_call = device.buffer_calls + 1U;
    device.fail_buffer_status = BufferStatus::allocation_failed;
    const auto allocationFailed = controller->startEditing(
        device, *prepared.viewport, controller->inputSnapshot());
    require(!allocationFailed.ok() &&
                allocationFailed.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        allocation_failed &&
                !controller->editing() &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({0U, 1U}) &&
                controller->currentBytes() == baselineBytes &&
                controller->revision() == baselineRevision &&
                !controller->dirty() && !controller->canUndo() &&
                !controller->canRedo() &&
                controller->inputSnapshot().inputEpoch == 0U &&
                prepared.viewport->aiSplineRevision() == baselineRevision &&
                *device.live_buffer_count == baselineLiveBuffers,
            "failed allocation keeps selection, history, and visible buffers");
    device.fail_buffer_call = 0U;

    PresentationTargetDescription targetDescription;
    targetDescription.width = 32U;
    targetDescription.height = 32U;
    targetDescription.format = TextureFormat::rgba8_unorm;
    FakeTarget target(targetDescription);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic drawDiagnostic;
    require(prepared.viewport->drawAndPresent(device, target, frame,
                                              drawDiagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.overlay_counts.back() == 2U,
            "failed start leaves selection markers drawable");

    const auto callsBeforeStart = device.buffer_calls;
    const auto started = controller->startEditing(
        device, *prepared.viewport, controller->inputSnapshot());
    require(started.ok() && started.changed && controller->editing() &&
                started.revision == baselineRevision &&
                started.replacedPassCount == 2U &&
                controller->configuration().selectedIndices.empty() &&
                !controller->overlays().selection.has_value() &&
                controller->inputSnapshot().inputEpoch == 1U &&
                controller->inputSnapshot().editing &&
                controller->currentBytes() == baselineBytes &&
                !controller->dirty() && !controller->canUndo() &&
                !controller->canRedo() &&
                device.buffer_calls == callsBeforeStart + 1U,
            "start clears selection without changing authoring history");
    require(prepared.viewport->drawAndPresent(device, target, frame,
                                              drawDiagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.overlay_counts.back() == 1U,
            "started edit mode omits cleared selection markers");

    const auto callsBeforeRepeatedStart = device.buffer_calls;
    const auto repeatedStart = controller->startEditing(
        device, *prepared.viewport, controller->inputSnapshot());
    require(repeatedStart.ok() && !repeatedStart.changed &&
                controller->editing() &&
                controller->inputSnapshot().inputEpoch == 1U &&
                device.buffer_calls == callsBeforeRepeatedStart,
            "repeated start with no selection performs no viewport work");
    const auto finished = controller->finishEditing(
        device, *prepared.viewport, controller->inputSnapshot());
    require(finished.ok() && finished.changed && !controller->editing() &&
                finished.replacedPassCount == 0U &&
                controller->inputSnapshot().inputEpoch == 2U &&
                !controller->inputSnapshot().editing &&
                finished.resultingInput == controller->inputSnapshot() &&
                controller->inputSnapshot().generation.publication == 1U &&
                prepared.viewport->aiSplineGenerationIdentity().has_value() &&
                controller->inputSnapshot().generation ==
                    *prepared.viewport->aiSplineGenerationIdentity() &&
                controller->revision() == baselineRevision &&
                controller->currentBytes() == baselineBytes &&
                device.buffer_calls == callsBeforeRepeatedStart,
            "short finish exits mode without model or viewport changes");
    const auto callsBeforeLifecycleAba = device.buffer_calls;
    const auto lifecycleAba = controller->cancelEditing(
        device, *prepared.viewport, initialInput);
    require(!lifecycleAba.ok() &&
                lifecycleAba.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        stale_input &&
                lifecycleAba.diagnostic.code ==
                    "workspace_ai_spline_controller_input_stale" &&
                controller->inputSnapshot().inputEpoch == 2U &&
                device.buffer_calls == callsBeforeLifecycleAba,
            "lifecycle input rejects a false-true-false ABA sequence");

    auto cancelCreated = createController();
    require(cancelCreated.ok(), "AI spline cancel controller creates");
    auto cancelController = std::move(cancelCreated.controller);
    FakeDevice cancelDevice;
    auto cancelPrepared = prepareController(cancelDevice, *cancelController);
    require(cancelPrepared.ok(), "AI spline cancel viewport prepares");
    const auto cancelled = cancelController->cancelEditing(
        cancelDevice, *cancelPrepared.viewport,
        cancelController->inputSnapshot());
    require(cancelled.ok() && cancelled.changed &&
                !cancelController->editing() &&
                cancelController->configuration().selectedIndices.empty() &&
                !cancelController->overlays().selection.has_value() &&
                cancelController->revision() == 0U &&
                !cancelController->dirty() &&
                !cancelController->canUndo() &&
                !cancelController->canRedo() &&
                cancelController->inputSnapshot().inputEpoch == 1U,
            "cancel clears selection without restoring the baseline");

    apex::app::WorkspaceAiSplineControllerConfiguration emptyConfiguration;
    auto emptyCreated = apex::app::WorkspaceAiSplineController::create(
        spline, std::move(emptyConfiguration));
    require(emptyCreated.ok(), "empty-selection lifecycle controller creates");
    auto emptyController = std::move(emptyCreated.controller);
    auto value = fixture();
    auto emptyRequest = request_for(value);
    emptyRequest.ai_spline_geometry = &emptyController->overlays().primary;
    emptyRequest.ai_spline_generation = emptyController->generation();
    emptyRequest.ai_spline_pipeline = ai_spline_pipeline(value);
    emptyRequest.ai_spline_selection_pipeline =
        ai_spline_camber_pipeline(value);
    FakeDevice emptyDevice;
    auto emptyPrepared = apex::app::prepareWorkspaceViewport(
        emptyDevice, value.document, emptyRequest);
    require(emptyPrepared.ok(),
            "controller viewport prepares a latent selection pipeline");
    const auto emptyBufferCalls = emptyDevice.buffer_calls;
    const auto emptyStarted = emptyController->startEditing(
        emptyDevice, *emptyPrepared.viewport,
        emptyController->inputSnapshot());
    require(emptyStarted.ok() && emptyStarted.changed &&
                emptyStarted.replacedPassCount == 0U &&
                emptyController->editing() &&
                emptyController->inputSnapshot().inputEpoch == 1U &&
                emptyStarted.resultingInput ==
                    emptyController->inputSnapshot() &&
                emptyController->inputSnapshot().generation.publication ==
                    0U &&
                emptyPrepared.viewport->aiSplineGenerationIdentity()
                    .has_value() &&
                emptyController->inputSnapshot().generation ==
                    *emptyPrepared.viewport->aiSplineGenerationIdentity() &&
                emptyDevice.buffer_calls == emptyBufferCalls,
            "empty-selection start changes only controller edit mode");

    apex::app::WorkspaceAiSplinePointSelectionRequest chainSelection;
    chainSelection.pointIndex = 0U;
    chainSelection.expected = emptyStarted.resultingInput;
    const auto chainSelected = emptyController->selectPoint(
        emptyDevice, *emptyPrepared.viewport, chainSelection);
    const auto chainFinished = emptyController->finishEditing(
        emptyDevice, *emptyPrepared.viewport,
        chainSelected.resultingInput);
    require(chainSelected.ok() && chainSelected.changed &&
                chainFinished.ok() && chainFinished.changed &&
                chainFinished.resultingInput.inputEpoch == 3U &&
                !chainFinished.resultingInput.editing &&
                chainFinished.resultingInput ==
                    emptyController->inputSnapshot() &&
                chainFinished.resultingInput.generation ==
                    *emptyPrepared.viewport->aiSplineGenerationIdentity(),
            "lifecycle and selection results chain their input snapshots");
}

void publishes_temporary_ai_spline_edit_transaction() {
    apex::formats::AiSpline spline;
    spline.source = "controller-temporary-edit.ai";
    spline.version = 7U;
    spline.points.resize(10U);
    spline.payloads.resize(10U);
    for (std::size_t index = 0U; index < spline.points.size(); ++index) {
        spline.points[index].position = {
            static_cast<float>(index) * 10.0F,
            static_cast<float>(index), 0.0F};
        spline.points[index].tag = static_cast<std::int32_t>(index);
    }
    auto created = apex::app::WorkspaceAiSplineController::create(
        spline, {});
    require(created.ok(), "temporary-edit controller creates");
    auto controller = std::move(created.controller);

    auto value = fixture();
    auto request = request_for(value);
    request.ai_spline_geometry = &controller->overlays().primary;
    request.ai_spline_generation = controller->generation();
    request.ai_spline_pipeline = ai_spline_pipeline(value);
    request.ai_spline_left_pipeline = ai_spline_side_pipeline(value);
    request.ai_spline_right_pipeline = ai_spline_side_pipeline(value);
    request.ai_spline_selection_pipeline = ai_spline_camber_pipeline(value);
    request.ai_spline_temporary_interpolation_pipeline =
        ai_spline_camber_pipeline(value);
    request.ai_spline_temporary_marker_pipeline =
        ai_spline_camber_pipeline(value);
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(),
            "temporary-edit viewport prepares latent dynamic passes");

    const auto baselineBytes = controller->currentBytes();
    const auto started = controller->startEditing(
        device, *prepared.viewport, controller->inputSnapshot());
    require(started.ok() && started.changed && controller->editing(),
            "temporary-edit mode starts");
    const auto select = [&](std::uint32_t index,
                            std::array<float, 3U> picked,
                            bool shift) {
        apex::app::WorkspaceAiSplinePointSelectionRequest selection;
        selection.pointIndex = index;
        selection.pickedPosition = picked;
        selection.shiftPressed = shift;
        selection.expected = controller->inputSnapshot();
        return controller->selectPoint(device, *prepared.viewport, selection);
    };
    const auto firstEndpoint =
        select(1U, {10.0F, 1.0F, 0.0F}, false);
    const auto prematureTemporary =
        select(2U, {20.5F, 500.0F, 1.0F}, true);
    const auto finalEndpoint =
        select(8U, {80.0F, 8.0F, 0.0F}, false);
    require(firstEndpoint.ok() && prematureTemporary.ok() &&
                !prematureTemporary.changed &&
                controller->temporaryEditPoints().empty() &&
                finalEndpoint.ok() &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({1U, 8U}),
            "Shift-click requires two ordered endpoint selections");

    const auto beforeFailedPoint = controller->inputSnapshot();
    device.fail_buffer_call = device.buffer_calls + 1U;
    device.fail_buffer_status = BufferStatus::upload_failed;
    const auto failedPoint = select(2U, {20.5F, 500.0F, 1.0F}, true);
    require(!failedPoint.ok() &&
                failedPoint.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        viewport_failed &&
                controller->temporaryEditPoints().empty() &&
                controller->inputSnapshot() == beforeFailedPoint &&
                controller->currentBytes() == baselineBytes,
            "failed temporary-point upload preserves all controller state");
    device.fail_buffer_call = 0U;

    const std::array<std::array<float, 3U>, 5U> picked = {{
        {20.5F, 500.0F, 1.0F},
        {30.5F, 500.0F, 2.0F},
        {40.5F, 500.0F, 1.0F},
        {50.5F, 500.0F, 0.0F},
        {20.5F, 500.0F, 1.0F},
    }};
    for (std::size_t index = 0U; index < picked.size(); ++index) {
        const auto added = select(static_cast<std::uint32_t>(index + 2U),
                                  picked[index], true);
        require(added.ok() && added.changed &&
                    added.temporaryPointCount == index + 1U,
                "Shift-click appends one temporary point");
        require(controller->overlays().temporaryMarkers.has_value(),
                "every temporary point has a visible marker pass");
        require(controller->overlays().temporaryInterpolation.has_value() ==
                    (index + 1U >= 5U),
                "temporary interpolation starts at five records");
    }
    require(controller->temporaryEditPoints()[0U].position ==
                std::array<float, 3U>{20.5F, 2.0F, 1.0F} &&
                controller->temporaryEditPoints()[4U].position ==
                    std::array<float, 3U>{20.5F, 6.0F, 1.0F},
            "temporary points preserve duplicate XZ clicks and snap Y to the closest raw point");
    require(controller->revision() == 0U &&
                controller->currentBytes() == baselineBytes &&
                !controller->canUndo(),
            "temporary points do not change model history");

    const auto boundary = select(1U, {21.5F, 2.0F, 1.0F}, false);
    require(boundary.ok() && !boundary.changed &&
                !controller->movableTemporaryPoint().has_value(),
            "a temporary-point distance of exactly one does not hit");
    const auto movable = select(2U, {20.5F, 2.0F, 1.0F}, false);
    require(movable.ok() && movable.changed &&
                controller->movableTemporaryPoint() == 0U &&
                controller->overlays().temporaryMarkers->sample_point_count ==
                    7U,
            "first strict temporary hit exposes recovered movable axes");
    const auto temporaryBeforeVisibility =
        controller->temporaryEditPoints();
    const auto selectionBeforeVisibility =
        controller->configuration().selectedIndices;
    const auto movableBeforeVisibility = controller->movableTemporaryPoint();
    const auto bytesBeforeVisibility = controller->currentBytes();
    const auto revisionBeforeVisibility = controller->revision();
    const bool undoBeforeVisibility = controller->canUndo();
    const bool redoBeforeVisibility = controller->canRedo();
    const auto sideVisible = controller->setSideVisibility(
        device, *prepared.viewport, true, false,
        controller->inputSnapshot());
    require(sideVisible.ok() && sideVisible.changed &&
                controller->editing() &&
                controller->temporaryEditPoints() ==
                    temporaryBeforeVisibility &&
                controller->configuration().selectedIndices ==
                    selectionBeforeVisibility &&
                controller->movableTemporaryPoint() ==
                    movableBeforeVisibility &&
                controller->currentBytes() == bytesBeforeVisibility &&
                controller->revision() == revisionBeforeVisibility &&
                controller->canUndo() == undoBeforeVisibility &&
                controller->canRedo() == redoBeforeVisibility &&
                controller->overlays().left.has_value() &&
                !controller->overlays().right.has_value(),
            "side visibility preserves temporary edit and history state");
    auto forgedOverlays = controller->overlays();
    const std::size_t forgedAxisEnd =
        static_cast<std::size_t>(
            forgedOverlays.temporaryMarkers->temporary_point_count) *
            2U +
        1U;
    forgedOverlays.temporaryMarkers->vertices[forgedAxisEnd].position[0U] +=
        1.0F;
    auto forgedGeneration = controller->generation();
    ++forgedGeneration.publication;
    const auto forged = prepared.viewport->replaceAiSplineOverlays(
        device, forgedOverlays,
        apex::app::WorkspaceViewportAiSplineGenerationTransition{
            controller->generation(), forgedGeneration});
    require(!forged.ok() &&
                forged.diagnostic.code ==
                    "workspace_viewport_ai_spline_temporary_axis_invalid" &&
                prepared.viewport->aiSplineGenerationIdentity() ==
                    controller->generation(),
            "forged temporary-axis length fails before viewport publication");
    const auto beforeMovement = controller->temporaryEditPoints();
    apex::app::WorkspaceAiSplineManualMovement movement;
    movement.forward = true;
    const auto moved = controller->moveSelectedByManualInput(
        device, *prepared.viewport, movement, controller->inputSnapshot());
    require(moved.ok() && moved.changed && controller->revision() == 0U &&
                controller->temporaryEditPoints()[0U].position[0U] >
                    beforeMovement[0U].position[0U] &&
                std::equal(controller->temporaryEditPoints().begin() + 1,
                           controller->temporaryEditPoints().end(),
                           beforeMovement.begin() + 1),
            "manual input moves only the selected temporary point");

    const auto lastEndpoint = controller->current().points[8U].position;
    const auto finished = controller->finishEditing(
        device, *prepared.viewport, controller->inputSnapshot());
    require(finished.ok() && finished.changed && !controller->editing() &&
                controller->temporaryEditPoints().empty() &&
                !controller->movableTemporaryPoint().has_value() &&
                !controller->overlays().temporaryInterpolation.has_value() &&
                !controller->overlays().temporaryMarkers.has_value() &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({1U, 8U}) &&
                controller->revision() == 1U && controller->canUndo() &&
                controller->current().points[8U].position == lastEndpoint,
            "five-point finish commits one revision, preserves selection, and leaves the final endpoint unchanged");

    const auto committedBytes = controller->currentBytes();
    require(controller->startEditing(device, *prepared.viewport,
                                     controller->inputSnapshot()).ok() &&
                select(8U, {80.0F, 8.0F, 0.0F}, false).ok() &&
                select(1U, {10.0F, 1.0F, 0.0F}, false).ok(),
            "reverse-range edit setup succeeds");
    for (std::size_t index = 0U; index < picked.size(); ++index)
        require(select(static_cast<std::uint32_t>(index + 2U), picked[index],
                       true)
                    .ok(),
                "reverse-range temporary point appends");
    const auto reverseFinished = controller->finishEditing(
        device, *prepared.viewport, controller->inputSnapshot());
    require(reverseFinished.ok() && reverseFinished.changed &&
                controller->revision() == 1U &&
                controller->currentBytes() == committedBytes &&
                controller->temporaryEditPoints().empty(),
            "reverse endpoint order clears temporary state without point writes");

    require(controller->startEditing(device, *prepared.viewport,
                                     controller->inputSnapshot()).ok() &&
                select(1U, {10.0F, 1.0F, 0.0F}, false).ok() &&
                select(8U, {80.0F, 8.0F, 0.0F}, false).ok() &&
                select(2U, picked[0U], true).ok(),
            "temporary cancel setup succeeds");
    const auto cancelled = controller->cancelEditing(
        device, *prepared.viewport, controller->inputSnapshot());
    require(cancelled.ok() && cancelled.changed && !controller->editing() &&
                controller->configuration().selectedIndices.empty() &&
                controller->temporaryEditPoints().empty() &&
                controller->currentBytes() == committedBytes &&
                controller->revision() == 1U,
            "cancel clears selection and temporary state without model writes");
}

void publishes_recovered_ai_spline_point_selection() {
    apex::formats::AiSpline spline;
    spline.source = "controller-selection.ai";
    spline.version = 7U;
    spline.points.resize(10U);
    spline.payloads.resize(10U);
    for (std::size_t index = 0U; index < spline.points.size(); ++index) {
        spline.points[index].position = {
            static_cast<float>(index) * 10.0F, 0.0F,
            static_cast<float>(index)};
        spline.points[index].tag = static_cast<std::int32_t>(index);
        spline.payloads[index].side0 = 1.0F;
        spline.payloads[index].side1 = 2.0F;
    }
    apex::app::WorkspaceAiSplineControllerConfiguration configuration;
    configuration.selectedIndices = {2U};
    auto created = apex::app::WorkspaceAiSplineController::create(
        spline, std::move(configuration));
    require(created.ok(), "AI spline selection controller creates");
    auto controller = std::move(created.controller);

    auto value = fixture();
    auto request = request_for(value);
    request.ai_spline_geometry = &controller->overlays().primary;
    request.ai_spline_generation = controller->generation();
    request.ai_spline_pipeline = ai_spline_pipeline(value);
    request.ai_spline_selection_geometry = &*controller->overlays().selection;
    request.ai_spline_selection_pipeline = ai_spline_camber_pipeline(value);
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "AI spline selection viewport prepares");
    auto stalePrepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(stalePrepared.ok(),
            "second AI spline selection viewport prepares");

    const auto baselineBytes = controller->currentBytes();
    const auto baselineGeneration = controller->generation();
    const auto baselineRevision = controller->revision();
    const auto select = [&](std::uint32_t index, bool control) {
        apex::app::WorkspaceAiSplinePointSelectionRequest selection;
        selection.pointIndex = index;
        selection.controlPressed = control;
        selection.expected = controller->inputSnapshot();
        return controller->selectPoint(device, *prepared.viewport, selection);
    };
    const auto preservesAuthoringState = [&]() {
        return controller->currentBytes() == baselineBytes &&
               controller->revision() == baselineRevision &&
               controller->generation().sameOwner(baselineGeneration) &&
               controller->generation().revision ==
                   baselineGeneration.revision &&
               !controller->dirty() && !controller->canUndo() &&
               !controller->canRedo();
    };

    const auto queuedMovementInput = controller->inputSnapshot();
    const auto replaced = select(6U, false);
    require(replaced.ok() && replaced.changed && replaced.selectionCount == 1U &&
                replaced.lastSelectedIndex == 6U &&
                replaced.normalizedPosition == 0.6F &&
                replaced.resultingInput.inputEpoch == 1U &&
                replaced.resultingInput.generation.publication == 1U &&
                replaced.resultingInput == controller->inputSnapshot() &&
                replaced.replacedPassCount == 2U &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({6U}) &&
                preservesAuthoringState(),
            "plain selection replaces indices without authoring changes");
    apex::app::WorkspaceAiSplineManualMovement queuedMovement;
    queuedMovement.forward = true;
    const auto pointSixBeforeQueuedMovement =
        controller->current().points[6U].position;
    const auto callsBeforeQueuedMovement = device.buffer_calls;
    const auto staleMovement = controller->moveSelectedByManualInput(
        device, *prepared.viewport, queuedMovement, queuedMovementInput);
    require(!staleMovement.ok() &&
                staleMovement.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        stale_input &&
                staleMovement.diagnostic.code ==
                    "workspace_ai_spline_controller_input_stale" &&
                controller->current().points[6U].position ==
                    pointSixBeforeQueuedMovement &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({6U}) &&
                device.buffer_calls == callsBeforeQueuedMovement &&
                preservesAuthoringState(),
            "queued movement cannot move a replacement selection");
    apex::app::WorkspaceAiSplinePointSelectionRequest staleViewportRequest;
    staleViewportRequest.pointIndex = 7U;
    staleViewportRequest.expected = controller->inputSnapshot();
    const auto callsBeforeStaleViewport = device.buffer_calls;
    const auto staleViewport = controller->selectPoint(
        device, *stalePrepared.viewport, staleViewportRequest);
    require(!staleViewport.ok() &&
                staleViewport.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        stale_state &&
                staleViewport.diagnostic.code ==
                    "workspace_ai_spline_controller_viewport_generation_mismatch" &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({6U}) &&
                device.buffer_calls == callsBeforeStaleViewport,
            "selection rejects a stale equal-model-revision viewport");
    const auto callsBeforeDuplicate = device.buffer_calls;
    const auto duplicate = select(6U, false);
    require(duplicate.ok() && !duplicate.changed &&
                duplicate.lastSelectedIndex == 6U &&
                duplicate.normalizedPosition == 0.6F &&
                device.buffer_calls == callsBeforeDuplicate &&
                controller->inputSnapshot().inputEpoch == 1U &&
                preservesAuthoringState(),
            "duplicate plain selection performs no viewport work");

    apex::app::WorkspaceAiSplinePointSelectionRequest queuedRange;
    queuedRange.pointIndex = 1U;
    queuedRange.controlPressed = true;
    queuedRange.expected = controller->inputSnapshot();
    require(select(4U, false).ok(),
            "intervening selection advances the input epoch");
    const auto callsBeforeSelectionAba = device.buffer_calls;
    const auto selectionAba = controller->selectPoint(
        device, *prepared.viewport, queuedRange);
    require(!selectionAba.ok() &&
                selectionAba.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        stale_input &&
                selectionAba.diagnostic.code ==
                    "workspace_ai_spline_controller_selection_input_stale" &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({4U}) &&
                device.buffer_calls == callsBeforeSelectionAba,
            "queued selection rejects an equal-generation anchor ABA");
    require(select(6U, false).ok(),
            "plain selection restores the tied-range fixture");

    const auto tiedRange = select(1U, true);
    require(tiedRange.ok() && tiedRange.changed &&
                tiedRange.lastSelectedIndex == 5U &&
                tiedRange.normalizedPosition == 0.5F &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({6U, 1U, 2U, 3U, 4U, 5U}),
            "Control tie walks clicked to anchor and returns the final entry");
    const auto forwardRange = select(8U, true);
    require(forwardRange.ok() && forwardRange.changed &&
                forwardRange.lastSelectedIndex == 8U &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>(
                        {6U, 1U, 2U, 3U, 4U, 5U, 7U, 8U}),
            "Control appends the shorter forward cyclic range");
    require(select(2U, false).ok(),
            "plain selection resets the reverse-range fixture");
    const auto reverseRange = select(8U, true);
    require(reverseRange.ok() && reverseRange.changed &&
                reverseRange.lastSelectedIndex == 1U &&
                reverseRange.normalizedPosition == 0.1F &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({2U, 8U, 9U, 0U, 1U}) &&
                preservesAuthoringState(),
            "Control appends the shorter wrapped reverse range");

    const auto callsBeforeInvalid = device.buffer_calls;
    auto invalidRequest =
        apex::app::WorkspaceAiSplinePointSelectionRequest{};
    invalidRequest.pointIndex = std::numeric_limits<std::uint32_t>::max();
    invalidRequest.expected = controller->inputSnapshot();
    const auto invalid = controller->selectPoint(
        device, *prepared.viewport, invalidRequest);
    auto boundaryRequest = invalidRequest;
    boundaryRequest.pointIndex =
        static_cast<std::uint32_t>(controller->current().points.size());
    const auto boundary = controller->selectPoint(
        device, *prepared.viewport, boundaryRequest);
    auto invalidSnapshotRequest = invalidRequest;
    invalidSnapshotRequest.pointIndex = 3U;
    invalidSnapshotRequest.expected = {};
    const auto invalidSnapshot = controller->selectPoint(
        device, *prepared.viewport, invalidSnapshotRequest);
    auto foreignCreated = apex::app::WorkspaceAiSplineController::create(
        spline, {});
    require(foreignCreated.ok(), "foreign selection controller creates");
    auto foreignRequest = invalidRequest;
    foreignRequest.pointIndex = 3U;
    foreignRequest.expected = foreignCreated.controller->inputSnapshot();
    const auto foreign = controller->selectPoint(
        device, *prepared.viewport, foreignRequest);
    auto staleRequest = invalidRequest;
    staleRequest.pointIndex = 3U;
    staleRequest.expected = controller->inputSnapshot();
    ++staleRequest.expected.generation.revision;
    const auto stale = controller->selectPoint(
        device, *prepared.viewport, staleRequest);
    require(!invalid.ok() && !boundary.ok() && !invalidSnapshot.ok() &&
                invalid.diagnostic.code ==
                    "workspace_ai_spline_controller_selection_index_invalid" &&
                boundary.diagnostic.code ==
                    "workspace_ai_spline_controller_selection_index_invalid" &&
                invalidSnapshot.diagnostic.code ==
                    "workspace_ai_spline_controller_selection_input_stale" &&
                !foreign.ok() && !stale.ok() &&
                foreign.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        stale_input &&
                stale.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        stale_input &&
                device.buffer_calls == callsBeforeInvalid &&
                preservesAuthoringState(),
            "selection rejects invalid, foreign, and stale input before upload");

    const auto selectionBeforeFailure =
        controller->configuration().selectedIndices;
    const auto liveBeforeFailure = *device.live_buffer_count;
    device.fail_buffer_call = device.buffer_calls + 2U;
    device.fail_buffer_status = BufferStatus::upload_failed;
    const auto uploadFailed = select(4U, false);
    require(!uploadFailed.ok() &&
                uploadFailed.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        viewport_failed &&
                controller->configuration().selectedIndices ==
                    selectionBeforeFailure &&
                *device.live_buffer_count == liveBeforeFailure &&
                preservesAuthoringState(),
            "failed selection upload keeps controller and visible buffers");
    device.fail_buffer_call = device.buffer_calls + 1U;
    device.fail_buffer_status = BufferStatus::allocation_failed;
    const auto allocationFailed = select(4U, false);
    require(!allocationFailed.ok() &&
                allocationFailed.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        allocation_failed &&
                controller->configuration().selectedIndices ==
                    selectionBeforeFailure &&
                *device.live_buffer_count == liveBeforeFailure &&
                preservesAuthoringState(),
            "failed selection allocation keeps controller and visible buffers");
    device.fail_buffer_call = 0U;
    require(select(4U, false).ok(), "selection retry succeeds");

    apex::app::WorkspaceAiSplinePointSelectionRequest beforeEdit;
    beforeEdit.pointIndex = 7U;
    beforeEdit.expected = controller->inputSnapshot();
    const auto started = controller->startEditing(
        device, *prepared.viewport, controller->inputSnapshot());
    const auto staleMode = controller->selectPoint(
        device, *prepared.viewport, beforeEdit);
    require(started.ok() && controller->editing() && !staleMode.ok() &&
                staleMode.diagnostic.code ==
                    "workspace_ai_spline_controller_selection_input_stale",
            "queued selection rejects an edit-mode transition");
    const auto editFirst = select(4U, false);
    const auto editControl = select(6U, true);
    require(editFirst.ok() && editControl.ok() &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({4U, 6U}) &&
                preservesAuthoringState(),
            "edit mode appends and ignores the Control range modifier");
    const auto cancelled = controller->cancelEditing(
        device, *prepared.viewport, controller->inputSnapshot());
    const auto emptyInput = controller->inputSnapshot();
    const auto liveBeforeLatentFailure = *device.live_buffer_count;
    device.fail_buffer_call = device.buffer_calls + 2U;
    device.fail_buffer_status = BufferStatus::upload_failed;
    const auto latentFailed = select(9U, true);
    require(cancelled.ok() && !latentFailed.ok() &&
                controller->configuration().selectedIndices.empty() &&
                !controller->overlays().selection.has_value() &&
                controller->inputSnapshot() == emptyInput &&
                *device.live_buffer_count == liveBeforeLatentFailure,
            "latent selection publication failure keeps the empty pass");
    device.fail_buffer_call = 0U;
    const auto controlEmpty = select(9U, true);
    require(cancelled.ok() && controlEmpty.ok() &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({9U}) &&
                controlEmpty.lastSelectedIndex == 9U &&
                preservesAuthoringState(),
            "Control with an empty view-mode selection adds one point");
}

void rejects_foreign_ai_spline_controller_generations() {
    apex::formats::AiSpline spline;
    spline.source = "controller-owner.ai";
    spline.version = 7U;
    spline.points.resize(4U);
    spline.payloads.resize(4U);
    for (std::size_t index = 0U; index < spline.points.size(); ++index) {
        spline.points[index].position = {
            static_cast<float>(index) * 10.0F, 0.0F, 0.0F};
        spline.points[index].tag = static_cast<std::int32_t>(index);
    }
    auto firstCreated = apex::app::WorkspaceAiSplineController::create(
        spline, {});
    auto secondCreated = apex::app::WorkspaceAiSplineController::create(
        spline, {});
    require(firstCreated.ok() && secondCreated.ok(),
            "independent AI spline controllers create");
    auto first = std::move(firstCreated.controller);
    auto second = std::move(secondCreated.controller);
    require(first->generation().valid() && second->generation().valid() &&
                first->generation().revision == 0U &&
                second->generation().revision == 0U &&
                !first->generation().sameOwner(second->generation()),
            "equal revisions retain distinct controller owners");

    auto value = fixture();
    auto request = request_for(value);
    request.ai_spline_geometry = &first->overlays().primary;
    request.ai_spline_generation = first->generation();
    request.ai_spline_pipeline = ai_spline_pipeline(value);
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "first controller viewport prepares");
    const auto bufferCalls = device.buffer_calls;
    const std::array<apex::authoring::AiSplinePointPositionEdit, 1U> edit{
        apex::authoring::AiSplinePointPositionEdit{0U, {1.0F, 0.0F, 0.0F}}};
    const auto foreign = second->setPointPositions(
        device, *prepared.viewport, edit, second->revision());
    require(!foreign.ok() &&
                foreign.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        stale_state &&
                foreign.diagnostic.code ==
                    "workspace_ai_spline_controller_viewport_generation_mismatch" &&
                first->revision() == 0U && second->revision() == 0U &&
                device.buffer_calls == bufferCalls,
            "equal-revision controller cannot publish to a foreign viewport");

    const auto secondGeneration = second->generation();
    const auto secondInput = second->inputSnapshot();
    apex::app::WorkspaceAiSplineController moved(std::move(*second));
    require(moved.generation() == secondGeneration &&
                moved.inputSnapshot() == secondInput &&
                moved.revision() == 0U,
            "controller move preserves its generation owner and input epoch");

    auto reprepareCreated =
        apex::app::WorkspaceAiSplineController::create(spline, {});
    require(reprepareCreated.ok(), "reprepare controller creates");
    auto reprepareController = std::move(reprepareCreated.controller);
    auto reprepareRequest = request_for(value);
    reprepareRequest.ai_spline_geometry =
        &reprepareController->overlays().primary;
    reprepareRequest.ai_spline_generation =
        reprepareController->generation();
    reprepareRequest.ai_spline_pipeline = ai_spline_pipeline(value);
    FakeDevice reprepareDevice;
    auto stalePrepared = apex::app::prepareWorkspaceViewport(
        reprepareDevice, value.document, reprepareRequest);
    require(stalePrepared.ok(), "initial reprepare viewport creates");
    const auto firstEdit = reprepareController->setPointPositions(
        reprepareDevice, *stalePrepared.viewport, edit,
        reprepareController->revision());
    require(firstEdit.ok() && firstEdit.revision == 1U,
            "controller advances before viewport reprepare");
    reprepareRequest.ai_spline_geometry =
        &reprepareController->overlays().primary;
    reprepareRequest.ai_spline_generation =
        reprepareController->generation();
    auto currentPrepared = apex::app::prepareWorkspaceViewport(
        reprepareDevice, value.document, reprepareRequest);
    require(currentPrepared.ok() &&
                currentPrepared.viewport->aiSplineGenerationIdentity()
                    .has_value() &&
                *currentPrepared.viewport->aiSplineGenerationIdentity() ==
                    reprepareController->generation(),
            "reprepared viewport retains current controller identity");
    const std::array<apex::authoring::AiSplinePointPositionEdit, 1U>
        secondEdit{apex::authoring::AiSplinePointPositionEdit{
            1U, {11.0F, 0.0F, 0.0F}}};
    const auto editedAgain = reprepareController->setPointPositions(
        reprepareDevice, *currentPrepared.viewport, secondEdit,
        reprepareController->revision());
    require(editedAgain.ok() && editedAgain.revision == 2U,
            "reprepared viewport accepts the next controller edit");
    const auto staleCalls = reprepareDevice.buffer_calls;
    const auto staleViewport = reprepareController->setPointPositions(
        reprepareDevice, *stalePrepared.viewport, secondEdit,
        reprepareController->revision());
    require(!staleViewport.ok() &&
                staleViewport.diagnostic.code ==
                    "workspace_ai_spline_controller_viewport_generation_mismatch" &&
                reprepareController->revision() == 2U &&
                reprepareDevice.buffer_calls == staleCalls,
            "controller rejects its stale pre-reprepare viewport");

    auto next = first->generation();
    next.revision = 1U;
    next.publication = 1U;
    const auto advanced = prepared.viewport->replaceAiSplineOverlays(
        device, first->overlays(),
        apex::app::WorkspaceViewportAiSplineGenerationTransition{
            first->generation(), next});
    require(advanced.ok() &&
                prepared.viewport->aiSplineGenerationIdentity().has_value() &&
                *prepared.viewport->aiSplineGenerationIdentity() == next,
            "generation transition accepts the next owned revision");

    const auto callsAfterAdvance = device.buffer_calls;
    const auto rollback = prepared.viewport->replaceAiSplineOverlays(
        device, first->overlays(),
        apex::app::WorkspaceViewportAiSplineGenerationTransition{
            next, first->generation()});
    require(!rollback.ok() &&
                rollback.diagnostic.code ==
                    "workspace_viewport_ai_spline_generation_transition_invalid" &&
                device.buffer_calls == callsAfterAdvance &&
                *prepared.viewport->aiSplineGenerationIdentity() == next,
            "generation transition rejects an ABA revision rollback");

    auto leap = next;
    leap.revision = 3U;
    leap.publication = 2U;
    const auto skipped = prepared.viewport->replaceAiSplineOverlays(
        device, first->overlays(),
        apex::app::WorkspaceViewportAiSplineGenerationTransition{next,
                                                                  leap});
    require(!skipped.ok() &&
                skipped.diagnostic.code ==
                    "workspace_viewport_ai_spline_generation_transition_invalid" &&
                device.buffer_calls == callsAfterAdvance,
            "generation transition rejects a skipped revision");

    auto foreignReplacement = moved.generation();
    foreignReplacement.revision = 2U;
    foreignReplacement.publication = 2U;
    const auto ownerChanged = prepared.viewport->replaceAiSplineOverlays(
        device, first->overlays(),
        apex::app::WorkspaceViewportAiSplineGenerationTransition{
            next, foreignReplacement});
    require(!ownerChanged.ok() &&
                ownerChanged.diagnostic.code ==
                    "workspace_viewport_ai_spline_generation_transition_invalid" &&
                device.buffer_calls == callsAfterAdvance,
            "generation transition rejects a replacement owner change");

    auto staleReplacement = moved.generation();
    staleReplacement.revision = 1U;
    staleReplacement.publication = 1U;
    const auto staleOwner = prepared.viewport->replaceAiSplineOverlays(
        device, first->overlays(),
        apex::app::WorkspaceViewportAiSplineGenerationTransition{
            moved.generation(), staleReplacement});
    require(!staleOwner.ok() &&
                staleOwner.diagnostic.code ==
                    "workspace_viewport_ai_spline_generation_stale" &&
                device.buffer_calls == callsAfterAdvance,
            "generation transition rejects a stale foreign expected owner");

    auto assignmentCreated =
        apex::app::WorkspaceAiSplineController::create(spline, {});
    require(assignmentCreated.ok(), "move-assignment controller creates");
    auto assignmentRequest = request_for(value);
    assignmentRequest.ai_spline_geometry =
        &assignmentCreated.controller->overlays().primary;
    assignmentRequest.ai_spline_generation =
        assignmentCreated.controller->generation();
    assignmentRequest.ai_spline_pipeline = ai_spline_pipeline(value);
    FakeDevice assignmentDevice;
    auto assignmentPrepared = apex::app::prepareWorkspaceViewport(
        assignmentDevice, value.document, assignmentRequest);
    require(assignmentPrepared.ok(),
            "move-assignment owner viewport prepares");
    const auto movedInput = moved.inputSnapshot();
    *assignmentCreated.controller = std::move(moved);
    require(assignmentCreated.controller->inputSnapshot() == movedInput,
            "controller move assignment preserves the input snapshot");
    const auto assignmentCalls = assignmentDevice.buffer_calls;
    const auto replacedOwnerEdit =
        assignmentCreated.controller->setPointPositions(
            assignmentDevice, *assignmentPrepared.viewport, edit,
            assignmentCreated.controller->revision());
    require(!replacedOwnerEdit.ok() &&
                replacedOwnerEdit.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        stale_state &&
                assignmentDevice.buffer_calls == assignmentCalls,
            "move assignment rejects a viewport for the replaced owner");

    auto invalidRequest = request;
    invalidRequest.ai_spline_generation =
        apex::app::WorkspaceViewportAiSplineGeneration{};
    const auto invalidPrepared = apex::app::prepareWorkspaceViewport(
        device, value.document, invalidRequest);
    require(!invalidPrepared.ok() &&
                invalidPrepared.diagnostic.code ==
                    "workspace_viewport_ai_spline_generation_invalid",
            "viewport rejects generation tracking without an owner");

    auto maximumRequest = request;
    auto maximum = first->generation();
    maximum.revision = std::numeric_limits<std::uint64_t>::max();
    maximumRequest.ai_spline_generation = maximum;
    auto maximumPrepared = apex::app::prepareWorkspaceViewport(
        device, value.document, maximumRequest);
    require(maximumPrepared.ok(), "maximum owned generation prepares");
    auto wrapped = maximum;
    wrapped.revision = 0U;
    wrapped.publication = 1U;
    const auto overflow = maximumPrepared.viewport->replaceAiSplineOverlays(
        device, first->overlays(),
        apex::app::WorkspaceViewportAiSplineGenerationTransition{maximum,
                                                                  wrapped});
    require(!overflow.ok() &&
                overflow.diagnostic.code ==
                    "workspace_viewport_ai_spline_generation_transition_invalid",
            "maximum generation cannot wrap to zero");

    auto maximumPublicationRequest = request;
    auto maximumPublication = first->generation();
    maximumPublication.publication =
        std::numeric_limits<std::uint64_t>::max();
    maximumPublicationRequest.ai_spline_generation = maximumPublication;
    auto maximumPublicationPrepared =
        apex::app::prepareWorkspaceViewport(
            device, value.document, maximumPublicationRequest);
    require(maximumPublicationPrepared.ok(),
            "maximum publication generation prepares");
    auto wrappedPublication = maximumPublication;
    wrappedPublication.publication = 0U;
    const auto publicationOverflow =
        maximumPublicationPrepared.viewport->replaceAiSplineOverlays(
            device, first->overlays(),
            apex::app::WorkspaceViewportAiSplineGenerationTransition{
                maximumPublication, wrappedPublication});
    require(!publicationOverflow.ok() &&
                publicationOverflow.diagnostic.code ==
                    "workspace_viewport_ai_spline_generation_transition_invalid",
            "maximum publication cannot wrap to zero");
}

void normalizes_legacy_and_rejects_unsafe_ai_spline_controller_candidates() {
    const auto legacy = legacyAiSplineFixture();
    apex::app::WorkspaceAiSplineControllerConfiguration legacyConfiguration;
    legacyConfiguration.showLeft = true;
    legacyConfiguration.showRight = true;
    legacyConfiguration.showCamber = true;
    legacyConfiguration.selectedIndices = {0U};
    auto legacyController =
        apex::app::WorkspaceAiSplineController::create(
            legacy, legacyConfiguration);
    require(legacyController.ok() && legacy.version == 2U &&
                legacyController.controller->current().version == 7U &&
                legacyController.controller->current().points.size() == 2U &&
                legacyController.controller->current().payloads.size() == 2U &&
                legacyController.controller->current().grid.has_value() &&
                legacyController.controller->overlays().left.has_value() &&
                legacyController.controller->overlays().right.has_value() &&
                legacyController.controller->overlays().selection.has_value() &&
                legacyController.controller->overlays().camber.has_value() &&
                legacyController.controller->revision() == 0U &&
                !legacyController.controller->dirty(),
            "version-2 controller input must normalize to a clean v7 session");
    const auto legacySave = legacyController.controller->buildSaveBytes(0U);
    require(legacySave.ok(),
            "version-2 controller baseline must build save bytes");
    const auto savedLegacy = apex::formats::parseAiSpline(
        legacySave.bytes, "saved-legacy-controller.ai");
    require(savedLegacy.version == 7U &&
                savedLegacy.points.size() == 2U && savedLegacy.grid.has_value(),
            "version-2 controller save must publish a complete v7 file");

    apex::authoring::AiSplineSessionLimits legacyLimits;
    legacyLimits.maxSnapshotModelBytes = sizeof(apex::formats::AiSpline);
    const auto limitedLegacyController =
        apex::app::WorkspaceAiSplineController::create(
            legacy, {}, legacyLimits);
    require(!limitedLegacyController.ok() &&
                limitedLegacyController.controller == nullptr &&
                limitedLegacyController.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::invalid_edit &&
                !limitedLegacyController.diagnostic.code.empty(),
            "version-2 conversion must inherit the session model limit");

    apex::formats::AiSpline spline;
    spline.source = "unsafe-controller.ai";
    spline.version = 7U;
    spline.points.resize(4U);
    spline.payloads.resize(4U);
    for (std::size_t index = 0U; index < spline.points.size(); ++index) {
        spline.points[index].position = {
            static_cast<float>(index) * 100.0F, 0.0F, 0.0F};
        spline.points[index].tag = static_cast<std::int32_t>(index);
    }
    apex::app::WorkspaceAiSplineControllerConfiguration invalidSelection;
    invalidSelection.selectedIndices = {99U};
    const auto rejectedSelection =
        apex::app::WorkspaceAiSplineController::create(spline,
                                                        invalidSelection);
    require(!rejectedSelection.ok() &&
                rejectedSelection.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        overlay_failed &&
                rejectedSelection.diagnostic.code ==
                    "workspace_ai_spline_selection_index_invalid",
            "controller creation rejects an invalid selected index");
    apex::app::WorkspaceAiSplineControllerConfiguration configuration;
    configuration.mode =
        apex::app::WorkspaceAiSplineDisplayMode::interpolated;
    auto created = apex::app::WorkspaceAiSplineController::create(
        std::move(spline), std::move(configuration));
    require(created.ok(), "interpolated controller baseline builds");
    auto controller = std::move(created.controller);

    auto value = fixture();
    auto request = request_for(value);
    request.ai_spline_geometry = &controller->overlays().primary;
    request.ai_spline_generation = controller->generation();
    request.ai_spline_pipeline = ai_spline_pipeline(value);
    FakeDevice device;
    auto prepared =
        apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(prepared.ok(), "interpolated controller viewport prepares");
    const auto bufferCalls = device.buffer_calls;
    const auto bytes = controller->currentBytes();

    const std::array<apex::authoring::AiSplinePointPositionEdit, 1U> edit{
        apex::authoring::AiSplinePointPositionEdit{3U, {0.0F, 0.0F, 0.0F}}};
    const auto rejected = controller->setPointPositions(
        device, *prepared.viewport, edit, controller->revision());
    require(!rejected.ok() &&
                rejected.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        overlay_failed &&
                controller->revision() == 0U && !controller->dirty() &&
                controller->currentBytes() == bytes &&
                device.buffer_calls == bufferCalls,
            "unsafe derived geometry cannot commit a model candidate");
}

void handles_degenerate_ai_spline_manual_forwards() {
    apex::formats::AiSpline spline;
    spline.source = "degenerate-manual.ai";
    spline.version = 7U;
    spline.points.resize(4U);
    spline.payloads.resize(4U);
    spline.points[0U].position = {0.0F, 0.0F, 0.0F};
    spline.points[1U].position = {0.0F, 5.0F, 0.0F};
    spline.points[2U].position = {100.0F, 0.0F, 0.0F};
    spline.points[3U].position = {200.0F, 0.0F, 0.0F};
    for (std::size_t index = 0U; index < spline.points.size(); ++index)
        spline.points[index].tag = static_cast<std::int32_t>(index);
    apex::app::WorkspaceAiSplineControllerConfiguration configuration;
    configuration.selectedIndices = {0U};
    auto created = apex::app::WorkspaceAiSplineController::create(
        spline, std::move(configuration));
    require(created.ok(), "zero-XZ-forward controller creates");
    auto controller = std::move(created.controller);

    auto value = fixture();
    auto request = request_for(value);
    request.ai_spline_geometry = &controller->overlays().primary;
    request.ai_spline_generation = controller->generation();
    request.ai_spline_pipeline = ai_spline_pipeline(value);
    request.ai_spline_selection_geometry =
        &*controller->overlays().selection;
    request.ai_spline_selection_pipeline = ai_spline_camber_pipeline(value);
    FakeDevice device;
    auto prepared =
        apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(prepared.ok(), "zero-XZ-forward viewport prepares");

    apex::app::WorkspaceAiSplineManualMovement horizontal;
    horizontal.forward = true;
    const auto bufferCalls = device.buffer_calls;
    const auto unchanged = controller->moveSelectedByManualInput(
        device, *prepared.viewport, horizontal, controller->inputSnapshot());
    require(unchanged.ok() && !unchanged.changed &&
                controller->revision() == 0U &&
                device.buffer_calls == bufferCalls,
            "zero cached forward suppresses horizontal movement");

    apex::app::WorkspaceAiSplineManualMovement vertical;
    vertical.up = true;
    const auto changed = controller->moveSelectedByManualInput(
        device, *prepared.viewport, vertical, controller->inputSnapshot());
    const float amount = apex::app::workspace_ai_spline_manual_speed *
                         apex::app::workspace_ai_spline_manual_fixed_delta;
    require(changed.ok() && changed.changed && changed.revision == 1U &&
                changed.replacedPassCount == 2U &&
                controller->current().points[0U].position ==
                    std::array<float, 3U>{0.0F, amount, 0.0F},
            "zero cached forward retains independent vertical movement");

    auto extreme = spline;
    extreme.source = "non-finite-forward.ai";
    extreme.points[0U].position = {0.0F, 0.0F, 0.0F};
    extreme.points[1U].position = {1.0e20F, 0.0F, 0.0F};
    const auto rejected =
        apex::app::WorkspaceAiSplineController::create(std::move(extreme), {});
    require(!rejected.ok() &&
                rejected.status ==
                    apex::app::WorkspaceAiSplineControllerStatus::
                        invalid_edit &&
                rejected.diagnostic.code ==
                    "workspace_ai_spline_controller_forward_non_finite",
            "controller rejects overflowed manual forward normalization");
}

void publishes_controller_through_d3d12_metadata_contract() {
    apex::formats::AiSpline spline;
    spline.source = "d3d12-controller.ai";
    spline.version = 7U;
    spline.points.resize(4U);
    spline.payloads.resize(4U);
    for (std::size_t index = 0U; index < spline.points.size(); ++index) {
        spline.points[index].position = {
            static_cast<float>(index) * 100.0F, 0.0F, 0.0F};
        spline.points[index].tag = static_cast<std::int32_t>(index);
    }
    apex::app::WorkspaceAiSplineControllerConfiguration configuration;
    configuration.selectedIndices = {1U};
    auto created = apex::app::WorkspaceAiSplineController::create(
        std::move(spline), std::move(configuration));
    require(created.ok(), "D3D12 contract controller creates");
    auto controller = std::move(created.controller);

    auto value = fixture();
    for (auto& module : value.modules) {
        module.format = PipelineShaderFormat::dxbc;
        module.bytes = dxbc_shader_bytes();
    }
    auto request = request_for(value);
    request.ai_spline_geometry = &controller->overlays().primary;
    request.ai_spline_generation = controller->generation();
    request.ai_spline_pipeline = ai_spline_pipeline(value);
    request.ai_spline_left_pipeline = ai_spline_side_pipeline(value);
    request.ai_spline_right_pipeline = ai_spline_side_pipeline(value);
    request.ai_spline_selection_geometry =
        &*controller->overlays().selection;
    request.ai_spline_selection_pipeline = ai_spline_camber_pipeline(value);
    request.ai_spline_temporary_interpolation_pipeline =
        ai_spline_camber_pipeline(value);
    request.ai_spline_temporary_marker_pipeline =
        ai_spline_camber_pipeline(value);
    FakeDevice device(Backend::D3D12);
    auto prepared =
        apex::app::prepareWorkspaceViewport(device, value.document, request);
    if (!prepared.ok())
        throw std::runtime_error(
            "D3D12 contract preparation failed: " +
            prepared.diagnostic.code + ": " + prepared.diagnostic.message);
    require(prepared.ok() &&
                prepared.viewport->backend() == Backend::D3D12,
            "D3D12 contract viewport prepares the controller baseline");
    const auto sidesVisible = controller->setSideVisibility(
        device, *prepared.viewport, true, true,
        controller->inputSnapshot());
    require(sidesVisible.ok() && sidesVisible.changed &&
                sidesVisible.replacedPassCount == 6U &&
                controller->overlays().left.has_value() &&
                controller->overlays().right.has_value(),
            "D3D12 contract publishes independent side visibility");

    apex::app::WorkspaceAiSplineManualMovement movement;
    movement.forward = true;
    const auto changed = controller->moveSelectedByManualInput(
        device, *prepared.viewport, movement, controller->inputSnapshot());
    require(changed.ok() && changed.changed && changed.replacedPassCount == 6U,
            "D3D12 contract accepts one manual controller generation");

    const auto publicationBeforeSelection =
        controller->generation().publication;
    apex::app::WorkspaceAiSplinePointSelectionRequest selection;
    selection.pointIndex = 3U;
    selection.expected = controller->inputSnapshot();
    const auto selected = controller->selectPoint(
        device, *prepared.viewport, selection);
    require(selected.ok() && selected.changed &&
                selected.replacedPassCount == 6U &&
                selected.resultingInput.generation.publication ==
                    publicationBeforeSelection + 1U &&
                controller->configuration().selectedIndices ==
                    std::vector<std::uint32_t>({3U}) &&
                controller->revision() == changed.revision,
            "D3D12 contract accepts one point selection transaction");

    require(controller->startEditing(device, *prepared.viewport,
                                     controller->inputSnapshot()).ok(),
            "D3D12 contract starts temporary editing");
    const auto select = [&](std::uint32_t index,
                            std::array<float, 3U> picked,
                            bool shift) {
        apex::app::WorkspaceAiSplinePointSelectionRequest point;
        point.pointIndex = index;
        point.pickedPosition = picked;
        point.shiftPressed = shift;
        point.expected = controller->inputSnapshot();
        return controller->selectPoint(device, *prepared.viewport, point);
    };
    require(select(0U, {0.0F, 0.0F, 0.0F}, false).ok() &&
                select(3U, {300.0F, 0.0F, 0.0F}, false).ok(),
            "D3D12 contract selects temporary endpoints");
    for (std::uint32_t index = 0U; index < 5U; ++index)
        require(select(1U,
                       {50.0F + static_cast<float>(index) * 40.0F, 0.0F,
                        static_cast<float>(index % 2U)},
                       true)
                    .ok(),
                "D3D12 contract publishes a temporary control point");
    require(controller->overlays().temporaryInterpolation.has_value() &&
                controller->overlays().temporaryMarkers.has_value(),
            "D3D12 contract owns both temporary overlay passes");

    FakeTarget target(request.presentation, Backend::D3D12);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::d3d12;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    require(
        prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
            device.overlay_buffers.size() == 6U,
        "D3D12 contract draws the accepted eight-pass controller generation");
}

void draws_selected_mesh_with_recovered_fade_boundary() {
  auto value = fixture();
  auto request = request_for(value);
  request.packets.selected_node = 1U;
  request.selected_mesh_pipeline = selected_mesh_pipeline(value);
  FakeDevice device;
  auto prepared =
      apex::app::prepareWorkspaceViewport(device, value.document, request);
  require(prepared.ok(), "selected-mesh viewport preparation succeeds");

  FakeTarget target(request.presentation);
  WorkspaceViewportFrameRequest frame;
  frame.camera.clip_space = CameraClipSpace::vulkan;
  frame.frame_constants = KsPerPixelFrameConstants{};
  Diagnostic diagnostic;
  frame.selected_mesh_elapsed_ms = 0U;
  require(
      prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
          WorkspaceViewportFrameStatus::ready,
      "selected-mesh initial frame draws");
  frame.selected_mesh_elapsed_ms = 2000U;
  require(
      prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
          WorkspaceViewportFrameStatus::ready,
      "selected-mesh zero-alpha boundary frame draws");
  frame.selected_mesh_elapsed_ms = 2001U;
  require(
      prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
          WorkspaceViewportFrameStatus::ready,
      "selected-mesh expired frame draws scene only");
  require(device.selected_mesh_counts == std::vector<std::size_t>({1U, 1U, 0U}),
          "selected mesh remains scheduled at 2000 ms and expires after it");

  std::vector<std::array<float, 4U>> colors;
  for (const auto &update : device.buffer_updates) {
    if (update.bytes.size() != sizeof(std::array<float, 4U>))
      continue;
    std::array<float, 4U> color{};
    std::memcpy(color.data(), update.bytes.data(), update.bytes.size());
    colors.push_back(color);
  }
  require(
      colors.size() == 2U &&
          colors[0] == std::array<float, 4U>{1.0F, 0.0F, 1.0F, 0.5F} &&
          colors[1] == std::array<float, 4U>{1.0F, 0.0F, 1.0F, 0.0F},
      "viewport uploads exact magenta fade colors before the selected pass");
}

void toggles_prepared_authoring_grid_per_frame() {
    auto value = fixture();
    auto request = request_for(value);
    request.authoring_overlay_pipeline = authoring_overlay_pipeline(value);
    request.grid_visible = true;
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "grid-only viewport preparation succeeds");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.overlay_counts == std::vector<std::size_t>({1U}),
            "prepared grid is visible by default");
    frame.grid_visible = false;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.overlay_counts ==
                    std::vector<std::size_t>({1U, 0U}),
            "frame override hides the prepared grid without rebuilding resources");
}

void rejects_unbound_selection_axis_requests() {
    auto value = fixture();
    auto request = request_for(value);
    request.authoring_overlay_pipeline = authoring_overlay_pipeline(value);
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(!prepared.ok() &&
                prepared.status == apex::app::WorkspaceViewportStatus::invalid &&
                prepared.diagnostic.code ==
                    "workspace_viewport_selection_axis_node_invalid",
            "selection-axis preparation requires a valid selected node");

    request.authoring_overlay_pipeline.reset();
    prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "ordinary viewport preparation succeeds");
    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    frame.selection_axis_world = apex::scene::identity_matrix;
    Diagnostic diagnostic;
    const auto status = prepared.viewport->drawAndPresent(
        device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code ==
                    "workspace_viewport_selection_axis_unprepared" &&
                device.draw_calls == 0U && device.present_calls == 0U,
            "frame axis override requires prepared axis resources");

    auto grid_request = request_for(value);
    grid_request.grid_visible = true;
    auto missing_grid_pipeline = apex::app::prepareWorkspaceViewport(
        device, value.document, grid_request);
    require(!missing_grid_pipeline.ok() &&
                missing_grid_pipeline.diagnostic.code ==
                    "workspace_viewport_grid_pipeline_missing",
            "visible grid requires explicit executable overlay modules");

    frame.selection_axis_world.reset();
    frame.grid_visible = true;
    diagnostic = {};
    const auto grid_status = prepared.viewport->drawAndPresent(
        device, target, frame, diagnostic);
    require(grid_status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "workspace_viewport_grid_unprepared" &&
                device.draw_calls == 0U && device.present_calls == 0U,
            "frame cannot enable an unprepared grid");

    auto view_axis_request = request_for(value);
    view_axis_request.view_axis_visible = true;
    auto missing_view_axis_pipeline = apex::app::prepareWorkspaceViewport(
        device, value.document, view_axis_request);
    require(!missing_view_axis_pipeline.ok() &&
                missing_view_axis_pipeline.diagnostic.code ==
                    "workspace_viewport_view_axis_pipeline_missing",
            "visible view axis requires explicit executable overlay modules");

    frame.grid_visible.reset();
    frame.view_axis_visible = true;
    diagnostic = {};
    const auto view_axis_status = prepared.viewport->drawAndPresent(
        device, target, frame, diagnostic);
    require(view_axis_status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code ==
                    "workspace_viewport_view_axis_unprepared" &&
                device.draw_calls == 0U && device.present_calls == 0U,
            "frame cannot enable an unprepared view axis");
}

void schedules_directional_shadows_before_color_and_reuses_maps() {
    auto value = fixture();
    value.module_set.directional_shadow_receiver = true;
    auto request = request_for(value);
    request.render.include_shadows = true;
    request.directional_shadow_receiver = true;
    apex::app::WorkspaceViewportDirectionalShadowOptions shadows;
    shadows.maps.lighting.map_size = 32U;
    shadows.opaque_pipeline = opaque_shadow_pipeline(value);
    request.directional_shadows = shadows;

    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok() && device.depth_calls == 4U,
            "receiver preparation owns main depth and three shadow maps");
    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera = valid_shadow_camera();
    const auto lighting = apex::app::evaluateWorkspaceViewportLighting(
        {"3_clear", 120.0F, 35.0F});
    require(lighting.ok(), "non-default viewport lighting evaluates");
    frame.frame_constants = lighting.frame_constants;
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.events == std::vector<std::string>(
                    {"shadow", "shadow", "shadow", "color", "present"}) &&
                device.depth_draw_counts ==
                    std::vector<std::size_t>({1U, 1U, 1U}) &&
                device.depth_clear_values ==
                    std::vector<float>({1.0F, 1.0F, 1.0F}) &&
                device.receiver_maps.size() == 1U,
            "viewport executes three cascades before receiver color and present");

    require(!device.buffer_updates.empty() &&
                device.buffer_updates.back().offset == 0U &&
                device.buffer_updates.back().bytes.size() ==
                    portable_frame_buffer_view_bytes,
            "receiver uploads one bounded frame-lighting record");
    KsPerPixelFrameConstants uploaded{};
    std::memcpy(&uploaded, device.buffer_updates.back().bytes.data(),
                sizeof(uploaded));
    require(uploaded.sun_direction == lighting.frame_constants.sun_direction &&
                uploaded.sun_color == lighting.frame_constants.sun_color &&
                uploaded.ambient_color == lighting.frame_constants.ambient_color &&
                uploaded.horizon_color == lighting.frame_constants.horizon_color &&
                uploaded.sky_color == lighting.frame_constants.sky_color &&
                uploaded.fog_color == lighting.frame_constants.fog_color &&
                uploaded.fog == lighting.frame_constants.fog &&
                uploaded.camera_position ==
                    std::array<float, 4U>{frame.camera.position[0],
                                          frame.camera.position[1],
                                          frame.camera.position[2], 0.0F},
            "receiver upload keeps evaluated weather and renderer-owned camera state");

    auto expected_shadow = shadows.maps.lighting;
    expected_shadow.eye = frame.camera.position;
    expected_shadow.target = {
        frame.camera.position[0] + frame.camera.forward[0],
        frame.camera.position[1] + frame.camera.forward[1],
        frame.camera.position[2] + frame.camera.forward[2]};
    expected_shadow.up = frame.camera.up;
    expected_shadow.fov_radians = frame.camera.fov_radians;
    expected_shadow.aspect = frame.camera.aspect;
    expected_shadow.near_plane = frame.camera.near_plane;
    expected_shadow.far_plane = frame.camera.far_plane;
    expected_shadow.sun_direction = lighting.evaluated.sun_direction;
    const auto expected_cascades = computeDirectionalShadowCascades(expected_shadow);
    const auto expected_matrix = convertDirectionalShadowCascadeMatrix(
        expected_cascades.cascades.front().matrix, CameraClipSpace::vulkan);
    require(expected_matrix.ok() && !device.depth_camera_matrices.empty() &&
                std::equal(expected_matrix.matrix.begin(),
                           expected_matrix.matrix.end(),
                           device.depth_camera_matrices.front().begin(),
                           [](float left, float right) {
                               return std::abs(left - right) < 1.0e-5F;
                           }),
            "shadow camera and receiver use the same evaluated sun direction");
    for (std::size_t cascade = 0U;
         cascade < directional_shadow_cascade_count; ++cascade) {
        require(device.receiver_maps.front()[cascade] ==
                    device.depth_targets[cascade],
                "receiver samples the retained map written by its cascade");
    }

    const auto retained_targets = device.depth_targets;
    frame.camera = valid_shadow_camera(1.0F);
    require(prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.depth_calls == 4U &&
                std::equal(retained_targets.begin(), retained_targets.end(),
                           device.depth_targets.begin() + 3U),
            "camera movement refreshes cascade matrices and reuses native map allocations");

    const auto shadow_before_bad_lighting = device.depth_batch_calls;
    const auto color_before_bad_lighting = device.draw_calls;
    const auto present_before_bad_lighting = device.present_calls;
    auto invalid_constants = lighting.frame_constants;
    invalid_constants.sun_direction[0] =
        std::numeric_limits<float>::quiet_NaN();
    frame.frame_constants = invalid_constants;
    require(prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "static_scene_frame_constants_non_finite" &&
                device.depth_batch_calls == shadow_before_bad_lighting &&
                device.draw_calls == color_before_bad_lighting &&
                device.present_calls == present_before_bad_lighting,
            "non-finite lighting fails before shadow, color, and present work");
    invalid_constants = lighting.frame_constants;
    invalid_constants.fog_color[1] =
        std::numeric_limits<float>::quiet_NaN();
    frame.frame_constants = invalid_constants;
    require(prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "static_scene_frame_constants_non_finite" &&
                device.depth_batch_calls == shadow_before_bad_lighting &&
                device.draw_calls == color_before_bad_lighting &&
                device.present_calls == present_before_bad_lighting,
            "non-finite fog lighting fails before shadow, color, and present work");
    invalid_constants = lighting.frame_constants;
    invalid_constants.sun_direction = {};
    frame.frame_constants = invalid_constants;
    require(prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "static_scene_frame_sun_direction_invalid" &&
                device.depth_batch_calls == shadow_before_bad_lighting &&
                device.draw_calls == color_before_bad_lighting &&
                device.present_calls == present_before_bad_lighting,
            "zero sun lighting fails before shadow, color, and present work");
    frame.frame_constants = lighting.frame_constants;

    const auto color_before_failure = device.draw_calls;
    const auto present_before_failure = device.present_calls;
    device.fail_depth_batch_call = device.depth_batch_calls + 2U;
    frame.camera = valid_shadow_camera(2.0F);
    require(prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::execution_failed &&
                diagnostic.code == "fake_shadow_failed" &&
                device.draw_calls == color_before_failure &&
                device.present_calls == present_before_failure,
            "cascade failure prevents receiver color and present");

    device.fail_depth_batch_call = 0U;
    const auto shadow_before_invalid = device.depth_batch_calls;
    frame.camera.fov_radians = std::numeric_limits<float>::quiet_NaN();
    require(prepared.viewport->drawAndPresent(
                device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "directional_shadow_refresh_camera_invalid" &&
                device.depth_batch_calls == shadow_before_invalid &&
                device.draw_calls == color_before_failure &&
                device.present_calls == present_before_failure,
            "invalid shadow camera fails before drawing or presenting");

    request.directional_shadows->opaque_pipeline.reset();
    FakeDevice staged_device;
    auto staged = apex::app::prepareWorkspaceViewport(
        staged_device, value.document, request);
    FakeTarget staged_target(request.presentation);
    frame.camera = valid_shadow_camera();
    require(staged.ok() && staged.viewport->drawAndPresent(
                staged_device, staged_target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                diagnostic.code == "directional_shadow_all_casters_staged" &&
                staged_device.depth_draw_counts ==
                    std::vector<std::size_t>({0U, 0U, 0U}) &&
                staged_device.draw_calls == 1U &&
                staged_device.present_calls == 1U,
            "staged caster branches remain labeled on a presented clear-map frame");
}

void prepares_and_draws_builtin_vulkan_source_viewport() {
    auto value = fixture();
    auto request = request_for(value);
    request.shader_modules = {};
    request.builtin_vulkan_source =
        BuiltinVulkanStockSourceSelector::ks_per_pixel;
    request.builtin_vulkan_source_sampler_settings = {4.0F, -2.5F};
    FakeDevice missing_shadow_device;
    const auto missing_shadows = apex::app::prepareWorkspaceViewport(
        missing_shadow_device, value.document, request);
    require(!missing_shadows.ok() &&
                missing_shadows.diagnostic.code ==
                    "workspace_viewport_stock_vulkan_source_shadows_missing",
            "a selected source owner requires viewport-owned shadow maps");
    request.directional_shadows =
        apex::app::WorkspaceViewportDirectionalShadowOptions{};
    request.directional_shadows->maps.lighting.map_size = 32U;
    request.directional_shadows->opaque_pipeline =
        opaque_shadow_pipeline(value);

    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok() &&
                prepared.viewport->preparation().resources != nullptr &&
                prepared.viewport->preparation().resources
                        ->stock_vulkan_source_program_count() == 1U &&
                device.sampler_descriptions.size() >= 3U &&
                device.sampler_descriptions[0].max_anisotropy == 4.0F &&
                device.sampler_descriptions[0].mip_lod_bias == -2.5F &&
                device.sampler_descriptions[1].compare == SamplerCompare::less,
            "viewport preparation forwards the Vulkan source selector and sampler settings");
    require(device.created_depth_attachments.size() == 4U,
            "source viewport owns main depth plus three directional cascades");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera = valid_shadow_camera();
    const auto evaluated = apex::app::evaluateWorkspaceViewportLighting({});
    const auto native_lighting =
        apex::app::buildWorkspaceViewportStockVulkanSourceLighting(
            evaluated.evaluated, request.presentation.width,
            request.presentation.height);
    require(native_lighting.has_value(),
            "evaluated weather builds the native source lighting record");
    frame.stock_vulkan_source_frame =
        apex::app::buildWorkspaceViewportStockVulkanSourceFrame(
            frame.camera, *native_lighting);
    require(frame.stock_vulkan_source_frame.has_value(),
            "the live camera builds the native source frame record");
    frame.frame_constants = evaluated.frame_constants;
    Diagnostic diagnostic;
    const auto source_status = prepared.viewport->drawAndPresent(
        device, target, frame, diagnostic);
    if (source_status != WorkspaceViewportFrameStatus::ready)
        throw std::runtime_error("native source draw: " + diagnostic.code +
                                 " " + diagnostic.message);
    require(device.events == std::vector<std::string>(
                {"shadow", "shadow", "shadow", "color", "present"}) &&
                device.source_shadow_maps.size() == 1U,
            "valid native source frame draws after refreshing the three cascades");
    require(device.source_shadow_maps.front() ==
                    std::array<const DepthAttachment *,
                               stock_ks_per_pixel_shadow_cascade_count>{
                        device.created_depth_attachments[1],
                        device.created_depth_attachments[2],
                        device.created_depth_attachments[3]},
            "source draw binds the viewport-owned directional cascades");

    auto expected_shadow = request.directional_shadows->maps.lighting;
    expected_shadow.eye = frame.camera.position;
    expected_shadow.target = {
        frame.camera.position[0] + frame.camera.forward[0],
        frame.camera.position[1] + frame.camera.forward[1],
        frame.camera.position[2] + frame.camera.forward[2]};
    expected_shadow.up = frame.camera.up;
    expected_shadow.fov_radians = frame.camera.fov_radians;
    expected_shadow.aspect = frame.camera.aspect;
    expected_shadow.near_plane = frame.camera.near_plane;
    expected_shadow.far_plane = ks_editor_shadow_range;
    expected_shadow.splits = ks_editor_shadow_splits;
    expected_shadow.sun_direction = {
        -frame.stock_vulkan_source_frame->lighting.light_direction[0],
        -frame.stock_vulkan_source_frame->lighting.light_direction[1],
        -frame.stock_vulkan_source_frame->lighting.light_direction[2]};
    const auto expected_cascades =
        computeDirectionalShadowCascades(expected_shadow);
    const auto expected_matrix = convertDirectionalShadowCascadeMatrix(
        expected_cascades.cascades.front().matrix,
        CameraClipSpace::vulkan);
    require(expected_matrix.ok() && !device.depth_camera_matrices.empty() &&
                std::equal(expected_matrix.matrix.begin(),
                           expected_matrix.matrix.end(),
                           device.depth_camera_matrices.front().begin(),
                           [](float left, float right) {
                               return std::abs(left - right) < 1.0e-5F;
                           }),
            "production weather and camera builders drive installed-editor source shadows");
}

void rejects_invalid_builtin_vulkan_source_frames_before_draw() {
    auto value = fixture();
    auto request = request_for(value);
    request.shader_modules = {};
    request.builtin_vulkan_source =
        BuiltinVulkanStockSourceSelector::ks_per_pixel;
    request.directional_shadows =
        apex::app::WorkspaceViewportDirectionalShadowOptions{};
    request.directional_shadows->maps.lighting.map_size = 32U;
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "source viewport setup for frame validation succeeds");
    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera = valid_shadow_camera();
    Diagnostic diagnostic;
    const auto shadow_before = device.depth_batch_calls;
    const auto color_before = device.draw_calls;
    const auto present_before = device.present_calls;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "static_scene_stock_vulkan_source_frame_missing" &&
                device.depth_batch_calls == shadow_before &&
                device.draw_calls == color_before &&
                device.present_calls == present_before,
            "source viewport requires a native frame before any draw or present");

    frame.stock_vulkan_source_frame = valid_source_frame(frame.camera);
    frame.stock_vulkan_source_frame->camera.near_plane =
        std::numeric_limits<float>::quiet_NaN();
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "static_scene_stock_vulkan_source_frame_invalid" &&
                device.depth_batch_calls == shadow_before &&
                device.draw_calls == color_before &&
                device.present_calls == present_before,
            "non-finite source camera records fail before shadow, color, or present work");

    frame.stock_vulkan_source_frame = valid_source_frame(frame.camera);
    frame.stock_vulkan_source_frame->lighting.light_direction[0] =
        std::numeric_limits<float>::quiet_NaN();
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "static_scene_stock_vulkan_source_frame_invalid" &&
                device.depth_batch_calls == shadow_before &&
                device.draw_calls == color_before &&
                device.present_calls == present_before,
            "non-finite source records fail before shadow, color, or present work");

    frame.stock_vulkan_source_frame = valid_source_frame(frame.camera);
    frame.stock_vulkan_source_frame->camera.view[0] += 1.0F;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code ==
                    "workspace_viewport_stock_vulkan_source_camera_mismatch" &&
                device.depth_batch_calls == shadow_before &&
                device.draw_calls == color_before &&
                device.present_calls == present_before,
            "source camera mismatch fails before shadow, color, or present work");
}

void preserves_portable_and_d3d12_viewport_paths() {
    auto value = fixture();
    auto portable_request = request_for(value);
    FakeDevice portable_device;
    auto portable = apex::app::prepareWorkspaceViewport(
        portable_device, value.document, portable_request);
    require(portable.ok() &&
                portable.viewport->preparation().resources
                        ->stock_vulkan_source_program_count() == 0U,
            "default viewport preparation remains portable");
    FakeTarget portable_target(portable_request.presentation);
    WorkspaceViewportFrameRequest portable_frame;
    portable_frame.camera = valid_shadow_camera();
    portable_frame.frame_constants = KsPerPixelFrameConstants{};
    portable_frame.stock_vulkan_source_frame =
        valid_source_frame(portable_frame.camera);
    Diagnostic diagnostic;
    require(portable.viewport->drawAndPresent(
                portable_device, portable_target, portable_frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "static_scene_stock_vulkan_source_frame_unexpected" &&
                portable_device.draw_calls == 0U &&
                portable_device.present_calls == 0U,
            "portable viewport rejects an unexpected native source frame");

    auto d3d_value = fixture();
    for (auto& module : d3d_value.modules) {
        module.format = PipelineShaderFormat::dxbc;
        module.bytes = dxbc_shader_bytes();
    }
    auto d3d_request = request_for(d3d_value);
    d3d_request.shader_modules = {};
    d3d_request.builtin_vulkan_source =
        BuiltinVulkanStockSourceSelector::ks_per_pixel;
    FakeDevice d3d_source_device(Backend::D3D12);
    auto d3d_source = apex::app::prepareWorkspaceViewport(
        d3d_source_device, d3d_value.document, d3d_request);
    require(!d3d_source.ok() &&
                d3d_source.status == apex::app::WorkspaceViewportStatus::unsupported &&
                d3d_source.diagnostic.code ==
                    "workspace_viewport_builtin_source_backend_unsupported" &&
                d3d_source_device.buffer_calls == 0U &&
                d3d_source_device.texture_calls == 0U &&
                d3d_source_device.depth_calls == 0U &&
                d3d_source_device.sampler_calls == 0U,
            "D3D12 source fallback rejects before viewport allocation");

    d3d_request = request_for(d3d_value);
    d3d_request.builtin_vulkan_source =
        BuiltinVulkanStockSourceSelector::ks_per_pixel;
    FakeDevice d3d_explicit_device(Backend::D3D12);
    auto d3d_explicit = apex::app::prepareWorkspaceViewport(
        d3d_explicit_device, d3d_value.document, d3d_request);
    require(d3d_explicit.ok() &&
                d3d_explicit.viewport->preparation().resources
                        ->stock_vulkan_source_program_count() == 0U,
            "matching D3D12 modules remain authoritative with selector enabled");
    FakeTarget d3d_target(d3d_request.presentation, Backend::D3D12);
    WorkspaceViewportFrameRequest d3d_frame;
    d3d_frame.camera = valid_shadow_camera(0.0F, CameraClipSpace::d3d12);
    d3d_frame.frame_constants = KsPerPixelFrameConstants{};
    require(d3d_explicit.viewport->drawAndPresent(
                d3d_explicit_device, d3d_target, d3d_frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                d3d_explicit_device.draw_calls == 1U &&
                d3d_explicit_device.present_calls == 1U,
            "explicit D3D12 modules retain the portable viewport draw path");
}

void rejects_invalid_d3d12_native_viewport_selection_before_allocation() {
    auto value = fixture();
    auto request = request_for(value);
    request.shader_modules = {};
    request.builtin_d3d12_native =
        BuiltinD3D12StockNativeSelector::ks_per_pixel_base;
    std::vector<StockMaterialD3D12NativeProgram> placeholder_programs = {
        {"ksPerPixel", {}}};
    request.builtin_d3d12_native_programs = placeholder_programs;

    FakeDevice vulkan_device;
    auto wrong_backend = apex::app::prepareWorkspaceViewport(
        vulkan_device, value.document, request);
    require(!wrong_backend.ok() &&
                wrong_backend.status ==
                    apex::app::WorkspaceViewportStatus::unsupported &&
                wrong_backend.diagnostic.code ==
                    "workspace_viewport_d3d12_native_backend_unsupported" &&
                vulkan_device.buffer_calls == 0U &&
                vulkan_device.texture_calls == 0U &&
                vulkan_device.depth_calls == 0U &&
                vulkan_device.sampler_calls == 0U,
            "D3D12 native selection rejects Vulkan before allocation");

    request.builtin_vulkan_source =
        BuiltinVulkanStockSourceSelector::ks_per_pixel;
    FakeDevice conflict_device(Backend::D3D12);
    auto conflict = apex::app::prepareWorkspaceViewport(
        conflict_device, value.document, request);
    require(!conflict.ok() &&
                conflict.diagnostic.code ==
                    "workspace_viewport_native_selector_conflict" &&
                conflict_device.buffer_calls == 0U &&
                conflict_device.texture_calls == 0U &&
                conflict_device.depth_calls == 0U,
            "native selector conflict rejects before allocation");

    request.builtin_vulkan_source =
        BuiltinVulkanStockSourceSelector::disabled;
    request.builtin_d3d12_native_programs = {};
    FakeDevice empty_program_device(Backend::D3D12);
    auto empty_programs = apex::app::prepareWorkspaceViewport(
        empty_program_device, value.document, request);
    require(!empty_programs.ok() &&
                empty_programs.diagnostic.code ==
                    "stock_material_d3d12_native_program_count_invalid" &&
                empty_program_device.buffer_calls == 0U &&
                empty_program_device.texture_calls == 0U &&
                empty_program_device.depth_calls == 0U,
            "empty native owner tables reject before viewport allocation");

    request.builtin_d3d12_native_programs = placeholder_programs;
    request.color_samples = 4U;
    FakeDevice multisample_device(Backend::D3D12);
    auto multisample = apex::app::prepareWorkspaceViewport(
        multisample_device, value.document, request);
    require(!multisample.ok() &&
                multisample.status ==
                    apex::app::WorkspaceViewportStatus::unsupported &&
                multisample.diagnostic.code ==
                    "workspace_viewport_d3d12_native_multisample_unsupported" &&
                multisample_device.buffer_calls == 0U &&
                multisample_device.texture_calls == 0U &&
                multisample_device.depth_calls == 0U,
            "native D3D12 multisampling rejects before allocation");

    request.builtin_d3d12_native =
        BuiltinD3D12StockNativeSelector::
            ks_per_pixel_alpha_to_coverage;
    placeholder_programs[0U].key = "ksPerPixelAT";
    request.builtin_d3d12_native_programs = placeholder_programs;
    FakeDevice alpha_multisample_device(Backend::D3D12);
    auto alpha_multisample = apex::app::prepareWorkspaceViewport(
        alpha_multisample_device, value.document, request);
    require(!alpha_multisample.ok() &&
                alpha_multisample.diagnostic.code ==
                    "stock_material_d3d12_native_program_invalid",
            "native D3D12 ksPerPixelAT accepts the four-sample viewport gate before owner validation");

    request.color_samples = 1U;
    FakeDevice alpha_one_sample_device(Backend::D3D12);
    auto alpha_one_sample = apex::app::prepareWorkspaceViewport(
        alpha_one_sample_device, value.document, request);
    require(!alpha_one_sample.ok() &&
                alpha_one_sample.diagnostic.code ==
                    "workspace_viewport_d3d12_native_multisample_unsupported" &&
                alpha_one_sample_device.buffer_calls == 0U &&
                alpha_one_sample_device.texture_calls == 0U &&
                alpha_one_sample_device.depth_calls == 0U,
            "native D3D12 ksPerPixelAT rejects one-sample viewports before allocation");

    request.builtin_d3d12_native =
        BuiltinD3D12StockNativeSelector::
            ks_per_pixel_base_and_alpha_to_coverage;
    request.color_samples = 4U;
    request.builtin_d3d12_native_programs = placeholder_programs;
    FakeDevice combined_program_device(Backend::D3D12);
    auto combined_programs = apex::app::prepareWorkspaceViewport(
        combined_program_device, value.document, request);
    require(!combined_programs.ok() &&
                combined_programs.diagnostic.code ==
                    "stock_material_d3d12_native_program_count_invalid" &&
                combined_program_device.buffer_calls == 0U &&
                combined_program_device.texture_calls == 0U &&
                combined_program_device.depth_calls == 0U,
            "combined native viewport requires both owners before allocation");

    request.builtin_d3d12_native =
        BuiltinD3D12StockNativeSelector::ks_per_pixel_base;
    placeholder_programs[0U].key = "ksPerPixel";
    request.builtin_d3d12_native_programs = placeholder_programs;

    request.color_samples = 1U;
    request.wireframe = true;
    FakeDevice wireframe_device(Backend::D3D12);
    auto wireframe = apex::app::prepareWorkspaceViewport(
        wireframe_device, value.document, request);
    require(!wireframe.ok() &&
                wireframe.status ==
                    apex::app::WorkspaceViewportStatus::unsupported &&
                wireframe.diagnostic.code ==
                    "workspace_viewport_d3d12_native_wireframe_unsupported" &&
                wireframe_device.buffer_calls == 0U &&
                wireframe_device.texture_calls == 0U &&
                wireframe_device.depth_calls == 0U,
            "native D3D12 wireframe rejects before allocation");

    request.wireframe = false;
    request.grid_visible = true;
    FakeDevice overlay_device(Backend::D3D12);
    auto overlay = apex::app::prepareWorkspaceViewport(
        overlay_device, value.document, request);
    require(!overlay.ok() &&
                overlay.diagnostic.code ==
                    "stock_material_d3d12_native_program_invalid" &&
                overlay_device.texture_calls == 1U &&
                overlay_device.depth_calls == 1U,
            "native D3D12 overlays reach retained-scene validation");
}

void shares_live_camera_visibility_with_directional_shadows() {
    auto value = fixture();
    auto& body = value.document.scene.snapshot.nodes[1U];
    body.local_bounds_source =
        apex::scene::LocalBoundsSource::kn5_serialized;
    body.local_bounds_center = {0.0F, 0.0F, 0.0F};
    body.local_bounds_radius = 0.0F;
    body.lod_out = 3.0F;
    value.module_set.directional_shadow_receiver = true;

    auto request = request_for(value);
    request.camera_mesh_filter = true;
    request.render.include_shadows = true;
    request.directional_shadow_receiver = true;
    apex::app::WorkspaceViewportDirectionalShadowOptions shadows;
    shadows.maps.lighting.map_size = 32U;
    shadows.opaque_pipeline = opaque_shadow_pipeline(value);
    request.directional_shadows = shadows;
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "live-filter shadow viewport prepares");

    CameraFrameRequest camera_request;
    camera_request.eye = {0.0F, 0.0F, 6.0F};
    camera_request.target = {0.0F, 0.0F, 0.0F};
    camera_request.aspect = 1.0F;
    camera_request.near_plane = 0.01F;
    camera_request.far_plane = 100.0F;
    camera_request.clip_space = CameraClipSpace::vulkan;
    const auto camera = build_camera_frame(camera_request);
    const auto lighting = apex::app::evaluateWorkspaceViewportLighting({});
    require(camera.ok() && lighting.ok(),
            "live-filter shadow frame inputs are valid");

    WorkspaceViewportFrameRequest frame;
    frame.camera = *camera.frame;
    frame.frame_constants = lighting.frame_constants;
    FakeTarget target(request.presentation);
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.depth_draw_counts ==
                    std::vector<std::size_t>({0U, 0U, 0U}) &&
                device.draw_counts == std::vector<std::size_t>({0U}) &&
                device.present_calls == 1U,
            "live mesh mask hides the same packet from color and shadow passes");
}

void retains_independent_shadowgen_visibility() {
    for (const Backend backend : {Backend::Vulkan, Backend::D3D12}) {
        auto value = shadow_only_fixture();
        if (backend == Backend::D3D12) {
            for (auto& module : value.modules) {
                module.format = PipelineShaderFormat::dxbc;
                module.bytes = dxbc_shader_bytes();
            }
        }
        value.module_set.directional_shadow_receiver = true;
        auto request = request_for(value);
        request.camera_mesh_filter = true;
        request.render.include_shadows = true;
        request.directional_shadow_receiver = true;
        apex::app::WorkspaceViewportDirectionalShadowOptions shadows;
        shadows.maps.lighting.map_size = 32U;
        shadows.opaque_pipeline = opaque_shadow_pipeline(value);
        request.directional_shadows = shadows;

        FakeDevice device(backend);
        auto prepared = apex::app::prepareWorkspaceViewport(
            device, value.document, request);
        const auto body_id = value.document.scene.snapshot.nodes[1U].id;
        const auto caster_id = value.document.scene.snapshot.nodes[3U].id;
        const auto visible_id = value.document.scene.snapshot.nodes[4U].id;
        require(prepared.ok() &&
                    prepared.viewport->renderPlan().items.size() == 2U &&
                    prepared.viewport->renderPlan().shadow_only_items.size() ==
                        1U &&
                    prepared.viewport->preparation().resources->draw_count() ==
                        3U &&
                    prepared.viewport->preparation().shadow_packet_order ==
                        std::vector<std::uint32_t>({0U, 2U, 1U}),
                "native viewport retains one invisible Shadowgen candidate");
        const auto prepared_buffers = device.buffer_calls;
        const auto prepared_textures = device.texture_calls;
        const auto prepared_depth = device.depth_calls;
        const auto prepared_samplers = device.sampler_calls;

        FakeTarget target(request.presentation, backend);
        WorkspaceViewportFrameRequest frame;
        frame.camera = valid_shadow_camera(
            0.0F, backend == Backend::Vulkan ? CameraClipSpace::vulkan
                                             : CameraClipSpace::d3d12);
        frame.frame_constants = KsPerPixelFrameConstants{};
        Diagnostic diagnostic;
        require(prepared.viewport->drawAndPresent(
                    device, target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                    device.draw_nodes.back() ==
                        std::vector<apex::scene::NodeId>{body_id, visible_id} &&
                    device.depth_nodes.size() == 3U &&
                    std::all_of(
                        device.depth_nodes.begin(), device.depth_nodes.end(),
                        [&](const auto& nodes) {
                            return nodes ==
                                   std::vector<apex::scene::NodeId>{
                                       body_id, caster_id, visible_id};
                        }),
                "Shadowgen ignores isVisible while color keeps the mesh hidden");

        const std::array<std::uint32_t, 3U> prepared_shadow_order = {
            0U, 1U, 2U};
        frame.shadow_packet_order = prepared_shadow_order;
        require(prepared.viewport->drawAndPresent(
                    device, target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                    device.draw_nodes.back() ==
                        std::vector<apex::scene::NodeId>{body_id, visible_id} &&
                    device.depth_nodes.back() ==
                        std::vector<apex::scene::NodeId>{body_id, visible_id,
                                                        caster_id},
                "an explicit full permutation overrides retained shadow order");
        frame.shadow_packet_order = {};

        const std::array<std::uint8_t, 3U> color_hidden = {0U, 0U, 0U};
        const std::array<std::uint8_t, 3U> all_shadow = {1U, 1U, 1U};
        frame.packet_visibility = color_hidden;
        require(prepared.viewport->drawAndPresent(
                    device, target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                    device.draw_nodes.back().empty() &&
                    device.depth_nodes.back().empty(),
                "the existing explicit mask keeps its shared compatibility behavior");

        frame.shadow_packet_visibility = all_shadow;
        require(prepared.viewport->drawAndPresent(
                    device, target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                    device.draw_nodes.back().empty() &&
                    device.depth_nodes.back() ==
                        std::vector<apex::scene::NodeId>{body_id, caster_id,
                                                        visible_id},
                "an independent shadow mask does not authorize color packets");

        const std::array<std::uint8_t, 3U> all_color = {1U, 1U, 1U};
        const std::array<std::uint8_t, 3U> caster_shadow = {0U, 0U, 1U};
        frame.packet_visibility = all_color;
        frame.shadow_packet_visibility = caster_shadow;
        require(prepared.viewport->drawAndPresent(
                    device, target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                    device.draw_nodes.back() ==
                        std::vector<apex::scene::NodeId>{body_id, visible_id} &&
                    device.depth_nodes.back() ==
                        std::vector<apex::scene::NodeId>{caster_id},
                "an independent color mask cannot show a shadow-only packet");

        const std::array<std::uint8_t, 1U> short_shadow = {1U};
        frame.shadow_packet_visibility = short_shadow;
        const auto updates_before_invalid = device.buffer_updates.size();
        const auto shadow_calls_before_invalid = device.depth_batch_calls;
        const auto color_calls_before_invalid = device.draw_calls;
        const auto presents_before_invalid = device.present_calls;
        require(prepared.viewport->drawAndPresent(
                    device, target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::invalid &&
                    diagnostic.code ==
                        "workspace_viewport_shadow_visibility_count_invalid" &&
                    device.buffer_updates.size() == updates_before_invalid &&
                    device.depth_batch_calls == shadow_calls_before_invalid &&
                    device.draw_calls == color_calls_before_invalid &&
                    device.present_calls == presents_before_invalid,
                "a short shadow mask fails before all mutable frame work");

        frame.shadow_packet_visibility = all_shadow;
        const std::array<std::uint8_t, 3U> invalid_color = {1U, 2U, 1U};
        frame.packet_visibility = invalid_color;
        require(prepared.viewport->drawAndPresent(
                    device, target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::invalid &&
                    diagnostic.code ==
                        "workspace_viewport_color_visibility_value_invalid" &&
                    device.buffer_updates.size() == updates_before_invalid &&
                    device.depth_batch_calls == shadow_calls_before_invalid &&
                    device.draw_calls == color_calls_before_invalid &&
                    device.present_calls == presents_before_invalid,
                "a non-binary color mask fails before all mutable frame work");

        frame.packet_visibility = all_color;
        frame.shadow_packet_visibility = all_shadow;
        const std::array<std::uint32_t, 1U> short_shadow_order = {0U};
        frame.shadow_packet_order = short_shadow_order;
        require(prepared.viewport->drawAndPresent(
                    device, target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::invalid &&
                    diagnostic.code ==
                        "workspace_viewport_shadow_order_count_invalid" &&
                    device.buffer_updates.size() == updates_before_invalid &&
                    device.depth_batch_calls == shadow_calls_before_invalid &&
                    device.draw_calls == color_calls_before_invalid &&
                    device.present_calls == presents_before_invalid,
                "a short shadow order fails before all mutable frame work");

        const std::array<std::uint32_t, 3U> duplicate_shadow_order = {
            0U, 0U, 2U};
        frame.shadow_packet_order = duplicate_shadow_order;
        require(prepared.viewport->drawAndPresent(
                    device, target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::invalid &&
                    diagnostic.code ==
                        "workspace_viewport_shadow_order_duplicate" &&
                    device.buffer_updates.size() == updates_before_invalid &&
                    device.depth_batch_calls == shadow_calls_before_invalid &&
                    device.draw_calls == color_calls_before_invalid &&
                    device.present_calls == presents_before_invalid,
                "a duplicate shadow order fails before all mutable frame work");

        const std::array<std::uint32_t, 3U> invalid_shadow_order = {
            0U, 1U, 3U};
        frame.shadow_packet_order = invalid_shadow_order;
        require(prepared.viewport->drawAndPresent(
                    device, target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::invalid &&
                    diagnostic.code ==
                        "workspace_viewport_shadow_order_index_invalid" &&
                    device.buffer_updates.size() == updates_before_invalid &&
                    device.depth_batch_calls == shadow_calls_before_invalid &&
                    device.draw_calls == color_calls_before_invalid &&
                    device.present_calls == presents_before_invalid,
                "an out-of-range shadow order fails before mutable frame work");
        frame.shadow_packet_order = {};

        std::vector<DrawPacket> malformed_packets(
            prepared.viewport->preparation().resources->prepared_packets().begin(),
            prepared.viewport->preparation().resources->prepared_packets().end());
        malformed_packets.front().flags.wireframe =
            !malformed_packets.front().flags.wireframe;
        frame.packet_visibility = color_hidden;
        frame.shadow_packet_visibility = color_hidden;
        frame.refreshed_packets = malformed_packets;
        require(prepared.viewport->drawAndPresent(
                    device, target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::invalid &&
                    diagnostic.code ==
                        "static_scene_frame_packet_contract_invalid" &&
                    device.buffer_updates.size() == updates_before_invalid &&
                    device.depth_batch_calls == shadow_calls_before_invalid &&
                    device.draw_calls == color_calls_before_invalid &&
                    device.present_calls == presents_before_invalid,
                "a malformed hidden packet fails before all mutable frame work");

        require(device.buffer_calls == prepared_buffers &&
                    device.texture_calls == prepared_textures &&
                    device.depth_calls == prepared_depth &&
                    device.sampler_calls == prepared_samplers,
                "independent pass masks reuse all prepared graphics resources");
    }
}

void accepts_track_and_car_lod_documents() {
    for (const auto kind : {apex::app::WorkspaceSessionKind::track,
                            apex::app::WorkspaceSessionKind::carLods}) {
        auto value = fixture();
        value.document.assembly.workspace.kind =
            kind == apex::app::WorkspaceSessionKind::track ? "track" : "carLods";
        value.document.scene.snapshot.workspace_kind = value.document.assembly.workspace.kind;
        auto request = request_for(value);
        FakeDevice device;
        auto prepared = apex::app::prepareWorkspaceViewport(
            device, value.document, request);
        require(prepared.ok() && !prepared.viewport->renderPlan().items.empty(),
                "track and car-LOD documents reach the viewport bridge");
    }
}

void selects_car_lod_roots_at_viewport_boundary() {
    auto value = car_lod_fixture();
    const auto authored_root_children =
        value.document.scene.snapshot.nodes[value.document.scene.snapshot.root].children;
    const auto authored_workspace_files = value.document.assembly.workspace.files.size();
    const bool authored_lod0_active = value.document.scene.snapshot.nodes[1U].active;

    auto request = request_for(value);
    request.render.camera_position = {0.0F, 0.0F, 5.0F};
    request.workspace.lod_bounds_center = apex::scene::Vector3{0.0F, 0.0F, 0.0F};
    request.workspace.lod_fov_degrees = 60.0F;
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(prepared.ok() && prepared.viewport->renderPlan().items.size() == 3U &&
                prepared.viewport->preparation().resources->draw_count() == 3U,
            "one viewport preparation retains every car LOD packet");
    const auto prepared_buffers = device.buffer_calls;
    const auto prepared_textures = device.texture_calls;
    const auto prepared_depth = device.depth_calls;
    const auto prepared_samplers = device.sampler_calls;
    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    const auto lod0_node = value.document.sceneBinding.file_root_nodes[0U];
    const auto lod1_node = value.document.sceneBinding.file_root_nodes[1U];
    const auto auxiliary_node = value.document.sceneBinding.file_root_nodes[2U];

    for (const auto& [distance, expected_node] :
         std::array<std::pair<float, apex::scene::NodeId>, 3U>{
             std::pair{14.999F, lod0_node},
             std::pair{15.0F, lod1_node},
             std::pair{15.001F, lod1_node}}) {
        frame.camera.position = {0.0F, 0.0F, distance};
        const auto status = prepared.viewport->drawAndPresent(
            device, target, frame, diagnostic);
        require(status == WorkspaceViewportFrameStatus::ready &&
                    device.draw_nodes.back() ==
                        std::vector<apex::scene::NodeId>{expected_node,
                                                         auxiliary_node},
                "frame-time mask selects one car LOD and keeps auxiliary geometry");
    }
    require(device.buffer_calls == prepared_buffers &&
                device.texture_calls == prepared_textures &&
                device.depth_calls == prepared_depth &&
                device.sampler_calls == prepared_samplers,
            "car LOD boundary changes reuse the prepared graphics resources");

    const std::array<std::uint8_t, 3U> explicit_lod1 = {0U, 1U, 0U};
    frame.camera.position = {0.0F, 0.0F, 5.0F};
    frame.packet_visibility = explicit_lod1;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.draw_nodes.back() ==
                    std::vector<apex::scene::NodeId>{lod1_node},
            "an explicit packet mask remains authoritative for a car LOD viewport");

    auto forced_request = request;
    forced_request.workspace.lod_index = 0U;
    FakeDevice forced_device;
    auto forced = apex::app::prepareWorkspaceViewport(
        forced_device, value.document, forced_request);
    FakeTarget forced_target(request.presentation);
    frame.packet_visibility = {};
    frame.camera.position = {0.0F, 0.0F, 30.0F};
    require(forced.ok() && forced.viewport->drawAndPresent(
                forced_device, forced_target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                forced_device.draw_nodes.back() ==
                    std::vector<apex::scene::NodeId>{lod0_node, auxiliary_node},
            "explicit viewport LOD index overrides camera distance");

    auto isolated_request = request;
    isolated_request.render.isolated = true;
    isolated_request.render.isolated_node = lod0_node;
    FakeDevice isolated_device;
    auto isolated = apex::app::prepareWorkspaceViewport(
        isolated_device, value.document, isolated_request);
    FakeTarget isolated_target(request.presentation);
    require(isolated.ok() && isolated.viewport->drawAndPresent(
                isolated_device, isolated_target, frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                isolated_device.draw_nodes.back() ==
                    std::vector<apex::scene::NodeId>{lod0_node},
            "exact mesh isolation bypasses the automatic workspace LOD mask");

    auto unknown_request = request;
    unknown_request.workspace.lod_index = 99U;
    FakeDevice unknown_device;
    auto unknown = apex::app::prepareWorkspaceViewport(
        unknown_device, value.document, unknown_request);
    require(!unknown.ok() && !unknown.viewport &&
                unknown.status == apex::app::WorkspaceViewportStatus::invalid &&
                unknown.diagnostic.code == "INVALID_LOD_SELECTION" &&
                unknown_device.texture_calls == 0U && unknown_device.depth_calls == 0U,
            "unknown viewport LOD index fails without a partial viewport");

    const auto draw_calls_before_invalid = device.draw_calls;
    const auto present_calls_before_invalid = device.present_calls;
    frame.camera.position[0U] = std::numeric_limits<float>::quiet_NaN();
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "workspace_viewport_lod_camera_invalid" &&
                device.draw_calls == draw_calls_before_invalid &&
                device.present_calls == present_calls_before_invalid,
            "non-finite frame LOD camera fails before drawing or presenting");

    require(value.document.scene.snapshot.nodes[value.document.scene.snapshot.root].children ==
                authored_root_children &&
                value.document.assembly.workspace.files.size() == authored_workspace_files &&
                value.document.scene.snapshot.nodes[1U].active == authored_lod0_active,
            "car LOD viewport preparation does not mutate the source document");
}

void applies_live_camera_mesh_filter_without_resource_rebuild() {
    auto value = fixture();
    auto& body = value.document.scene.snapshot.nodes[1U];
    body.local_bounds_center = {0.0F, 0.0F, 0.0F};
    body.local_bounds_radius = 0.0F;
    body.local_bounds_source =
        apex::scene::LocalBoundsSource::kn5_serialized;
    body.lod_in = 0.0F;
    body.lod_out = 3.0F;

    auto request = request_for(value);
    request.camera_mesh_filter = true;
    request.render.camera_position = {0.0F, 0.0F, 100.0F};
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok() && prepared.viewport->renderPlan().items.size() == 1U &&
                prepared.viewport->renderPlan().items.front().camera_mesh_filter.has_value(),
            "live camera filter preparation retains an initially out-of-LOD packet");
    const auto prepared_buffers = device.buffer_calls;
    const auto prepared_textures = device.texture_calls;
    const auto prepared_depth = device.depth_calls;
    const auto prepared_samplers = device.sampler_calls;

    const auto camera_at = [](float distance, float fov_radians) {
        CameraFrameRequest camera_request;
        camera_request.eye = {0.0F, 0.0F, distance};
        camera_request.target = {0.0F, 0.0F, 0.0F};
        camera_request.fov_radians = fov_radians;
        camera_request.aspect = 1.0F;
        camera_request.near_plane = 0.1F;
        camera_request.far_plane = 100.0F;
        camera_request.clip_space = CameraClipSpace::vulkan;
        const auto result = build_camera_frame(camera_request);
        if (!result.ok()) throw std::runtime_error("live filter camera fixture failed");
        return *result.frame;
    };

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera = camera_at(5.0F, 0.7853981633974483F);
    frame.frame_constants = KsPerPixelFrameConstants{};
    CameraMeshFilterRequest direct_filter;
    direct_filter.renderable = *prepared.viewport->renderPlan()
                                    .items.front().camera_mesh_filter;
    direct_filter.world_matrix = prepared.viewport->preparation()
                                     .resources->prepared_packets().front()
                                     .world_matrix;
    direct_filter.camera = &frame.camera;
    require(camera_mesh_filter_visible(direct_filter).visible(),
            "prepared live camera descriptor passes the direct filter");
    Diagnostic diagnostic;
    const auto near_status = prepared.viewport->drawAndPresent(
        device, target, frame, diagnostic);
    require(near_status == WorkspaceViewportFrameStatus::ready,
            diagnostic.code.empty() ? "near-side live camera frame is valid"
                                    : diagnostic.code.c_str());
    require(device.draw_counts.back() == 1U,
            "live camera filter includes the near-side LOD boundary packet");

    frame.camera = camera_at(6.0F, 0.7853981633974483F);
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.draw_counts.back() == 0U,
            "live camera movement removes a packet beyond its scaled far LOD");

    frame.camera = camera_at(6.0F, 0.3490658503988659F);
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.draw_counts.back() == 1U,
            "live camera filter uses the frame FOV instead of preparation FOV");
    require(device.buffer_calls == prepared_buffers &&
                device.texture_calls == prepared_textures &&
                device.depth_calls == prepared_depth &&
                device.sampler_calls == prepared_samplers,
            "live camera filter changes reuse prepared graphics resources");

    const auto prepared_packets =
        prepared.viewport->preparation().resources->prepared_packets();
    std::vector<DrawPacket> refreshed_packets(
        prepared_packets.begin(), prepared_packets.end());
    refreshed_packets.front().world_matrix[12] = 100.0F;
    frame.camera = camera_at(5.0F, 0.7853981633974483F);
    frame.refreshed_packets = refreshed_packets;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.draw_counts.back() == 0U,
            "live camera filter uses a refreshed packet world matrix");
    frame.refreshed_packets = {};

    const std::array<std::uint8_t, 1U> explicit_visible = {1U};
    frame.camera = camera_at(50.0F, 0.7853981633974483F);
    frame.packet_visibility = explicit_visible;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.draw_counts.back() == 1U,
            "explicit packet visibility remains authoritative over live filtering");

    frame.packet_visibility = {};
    frame.camera = camera_at(5.0F, 0.7853981633974483F);
    frame.camera.view_projection[0] =
        std::numeric_limits<float>::quiet_NaN();
    const auto draws_before_invalid = device.draw_calls;
    const auto presents_before_invalid = device.present_calls;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code ==
                    "workspace_viewport_color_camera_mesh_filter_invalid" &&
                device.draw_calls == draws_before_invalid &&
                device.present_calls == presents_before_invalid,
            "invalid live frustum fails before draw and present work");
}

void updates_webgl_compatible_transparent_order_per_frame() {
    auto value = transparent_order_fixture();
    const auto body_id = value.document.scene.snapshot.nodes[1U].id;
    const auto near_id = value.document.scene.snapshot.nodes[3U].id;
    const auto far_id = value.document.scene.snapshot.nodes[4U].id;
    auto request = request_for(value);
    request.webgl_live_transparent_order = true;
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok() &&
                prepared.viewport->preparation().resources->draw_count() == 3U,
            "WebGL-compatible live transparent-order viewport prepares");
    const auto prepared_buffers = device.buffer_calls;
    const auto prepared_textures = device.texture_calls;
    const auto prepared_depth = device.depth_calls;
    const auto prepared_samplers = device.sampler_calls;

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.camera.position = {0.0F, 0.0F, 0.0F};
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.draw_nodes.back() ==
                    std::vector<apex::scene::NodeId>{body_id, far_id, near_id},
            "transparent packets sort back-to-front from the current camera");

    frame.camera.position = {0.0F, 0.0F, 10.0F};
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.draw_nodes.back() ==
                    std::vector<apex::scene::NodeId>{body_id, near_id, far_id},
            "camera movement reverses transparent order without re-preparation");

    const auto prepared_packets =
        prepared.viewport->preparation().resources->prepared_packets();
    std::vector<DrawPacket> refreshed_packets(prepared_packets.begin(),
                                               prepared_packets.end());
    const auto near_packet = std::find_if(
        refreshed_packets.begin(), refreshed_packets.end(),
        [&](const DrawPacket& packet) { return packet.node == near_id; });
    require(near_packet != refreshed_packets.end(),
            "transparent-order fixture retains the near packet");
    near_packet->world_matrix[14U] = 20.0F;
    frame.camera.position = {0.0F, 0.0F, 0.0F};
    frame.refreshed_packets = refreshed_packets;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.draw_nodes.back() ==
                    std::vector<apex::scene::NodeId>{body_id, near_id, far_id},
            "refreshed world transforms update order in prepared packet-index space");

    require(device.buffer_calls == prepared_buffers &&
                device.texture_calls == prepared_textures &&
                device.depth_calls == prepared_depth &&
                device.sampler_calls == prepared_samplers,
            "live transparent ordering reuses all prepared graphics resources");

    near_packet->world_matrix[14U] =
        std::numeric_limits<float>::quiet_NaN();
    frame.refreshed_packets = refreshed_packets;
    const auto draws_before_invalid = device.draw_calls;
    const auto presents_before_invalid = device.present_calls;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code ==
                    "static_scene_frame_packet_contract_invalid" &&
                device.draw_calls == draws_before_invalid &&
                device.present_calls == presents_before_invalid,
            "a non-finite refreshed transform fails before draw and present work");

    auto d3d_value = transparent_order_fixture();
    for (auto& module : d3d_value.modules) {
        module.format = PipelineShaderFormat::dxbc;
        module.bytes = dxbc_shader_bytes();
    }
    auto d3d_request = request_for(d3d_value);
    d3d_request.webgl_live_transparent_order = true;
    FakeDevice d3d_device(Backend::D3D12);
    auto d3d_prepared = apex::app::prepareWorkspaceViewport(
        d3d_device, d3d_value.document, d3d_request);
    FakeTarget d3d_target(d3d_request.presentation, Backend::D3D12);
    WorkspaceViewportFrameRequest d3d_frame;
    d3d_frame.camera.clip_space = CameraClipSpace::d3d12;
    d3d_frame.camera.position = {0.0F, 0.0F, 0.0F};
    d3d_frame.frame_constants = KsPerPixelFrameConstants{};
    const auto d3d_body = d3d_value.document.scene.snapshot.nodes[1U].id;
    const auto d3d_near = d3d_value.document.scene.snapshot.nodes[3U].id;
    const auto d3d_far = d3d_value.document.scene.snapshot.nodes[4U].id;
    require(d3d_prepared.ok() &&
                d3d_prepared.viewport->drawAndPresent(
                    d3d_device, d3d_target, d3d_frame, diagnostic) ==
                    WorkspaceViewportFrameStatus::ready &&
                d3d_device.draw_nodes.back() ==
                    std::vector<apex::scene::NodeId>{d3d_body, d3d_far,
                                                     d3d_near},
            "D3D12 consumes the same backend-neutral live color order");
}

void combines_workspace_lod_and_live_mesh_visibility() {
    auto value = car_lod_fixture();
    const auto lod0_node = value.document.sceneBinding.file_root_nodes[0U];
    const auto lod1_node = value.document.sceneBinding.file_root_nodes[1U];
    const auto auxiliary_node = value.document.sceneBinding.file_root_nodes[2U];
    for (const auto node_id : {lod0_node, lod1_node, auxiliary_node}) {
        auto& node = value.document.scene.snapshot.nodes[
            static_cast<std::size_t>(node_id)];
        node.local_bounds_center = {0.0F, 0.0F, 0.0F};
        node.local_bounds_radius = 0.0F;
        node.local_bounds_source =
            apex::scene::LocalBoundsSource::kn5_serialized;
    }
    auto& auxiliary = value.document.scene.snapshot.nodes[
        static_cast<std::size_t>(auxiliary_node)];
    auxiliary.lod_in = 100.0F;
    auxiliary.lod_out = 200.0F;

    auto request = request_for(value);
    request.camera_mesh_filter = true;
    request.workspace.lod_bounds_center =
        apex::scene::Vector3{0.0F, 0.0F, 0.0F};
    request.workspace.lod_fov_degrees = 60.0F;
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok() && prepared.viewport->renderPlan().items.size() == 3U,
            "combined visibility preparation retains all three packets");

    CameraFrameRequest camera_request;
    camera_request.eye = {0.0F, 0.0F, 15.0F};
    camera_request.target = {0.0F, 0.0F, 0.0F};
    camera_request.fov_radians = 1.0471975511965976F;
    camera_request.aspect = 1.0F;
    camera_request.near_plane = 0.1F;
    camera_request.far_plane = 100.0F;
    camera_request.clip_space = CameraClipSpace::vulkan;
    const auto camera = build_camera_frame(camera_request);
    require(camera.ok(), "combined visibility camera fixture is valid");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera = *camera.frame;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.draw_nodes.back() ==
                    std::vector<apex::scene::NodeId>{lod1_node},
            "workspace LOD and live mesh masks combine with logical AND");

    const std::array<std::uint8_t, 3U> explicit_auxiliary = {0U, 0U, 1U};
    frame.packet_visibility = explicit_auxiliary;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready &&
                device.draw_nodes.back() ==
                    std::vector<apex::scene::NodeId>{auxiliary_node},
            "explicit packet visibility bypasses both automatic masks");
}

void resolves_preview_state_without_mutating_document() {
    auto value = fixture();
    const auto body_id = value.document.scene.snapshot.nodes[1U].id;
    const bool authored_active =
        value.document.scene.snapshot.find_node(body_id)->active;
    auto request = request_for(value);
    request.workspace.lod_bounds_center = apex::scene::Vector3{0.0F, 0.0F, 0.0F};
    request.workspace.driver_cockpit = true;
    const std::array<std::string, 1U> hidden_names = {"HIDDEN"};
    request.workspace.driver_hidden_names = hidden_names;
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok() && prepared.viewport->renderPlan().items.size() == 1U &&
                value.document.scene.snapshot.find_node(body_id)->active == authored_active,
            "workspace LOD and preview resolution do not mutate the source document");
}

void rejects_staged_fbx_before_backend_allocation() {
    auto ready_value = fixture();
    auto ready_request = request_for(ready_value);
    apex::app::FbxPreviewDocumentResult ready;
    ready.status = apex::app::FbxPreviewDocumentStatus::ready;
    ready.document = std::move(ready_value.document);
    FakeDevice ready_device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        ready_device, ready, ready_request);
    require(prepared.ok() && ready_device.buffer_calls > 0U &&
                ready_device.texture_calls > 0U &&
                ready_device.depth_calls > 0U,
            "ready FBX enters the shared workspace viewport");

    apex::app::FbxPreviewDocumentResult staged;
    staged.status = apex::app::FbxPreviewDocumentStatus::staged;
    staged.document = apex::app::WorkspaceSessionDocument{};
    FakeDevice device;
    WorkspaceViewportPrepareRequest request;
    const auto rejected = apex::app::prepareWorkspaceViewport(
        device, staged, request);
    require(rejected.status ==
                apex::app::WorkspaceViewportStatus::unsupported &&
                rejected.diagnostic.code ==
                    "fbx_preview_document_staged" &&
                device.buffer_calls == 0U && device.texture_calls == 0U &&
                device.depth_calls == 0U,
            "staged FBX is rejected before backend allocation");

    staged.status = apex::app::FbxPreviewDocumentStatus::invalid_request;
    staged.document.reset();
    const auto invalid = apex::app::prepareWorkspaceViewport(
        device, staged, request);
    require(invalid.status == apex::app::WorkspaceViewportStatus::invalid &&
                invalid.diagnostic.code ==
                    "fbx_preview_document_invalid" &&
                device.buffer_calls == 0U && device.texture_calls == 0U &&
                device.depth_calls == 0U,
            "invalid FBX document is rejected before backend allocation");
}

void rejects_invalid_inputs() {
    auto value = fixture();
    FakeDevice device;
    auto request = request_for(value);

    request.directional_shadow_receiver = true;
    auto missing_shadow_configuration = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(!missing_shadow_configuration.ok() &&
                missing_shadow_configuration.status ==
                    apex::app::WorkspaceViewportStatus::invalid &&
                missing_shadow_configuration.diagnostic.code ==
                    "workspace_viewport_directional_shadow_configuration_invalid" &&
                device.texture_calls == 0U && device.depth_calls == 0U,
            "receiver without a shadow schedule fails before allocation");

    request = request_for(value);
    request.directional_shadows =
        apex::app::WorkspaceViewportDirectionalShadowOptions{};
    auto unexpected_shadow_configuration = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(!unexpected_shadow_configuration.ok() &&
                unexpected_shadow_configuration.diagnostic.code ==
                    "workspace_viewport_directional_shadow_configuration_invalid" &&
                device.texture_calls == 0U && device.depth_calls == 0U,
            "shadow schedule without receiver modules fails before allocation");

    value.module_set.directional_shadow_receiver = true;
    request = request_for(value);
    request.directional_shadow_receiver = true;
    apex::app::WorkspaceViewportDirectionalShadowOptions invalid_shadows;
    invalid_shadows.opaque_pipeline = opaque_shadow_pipeline(value);
    invalid_shadows.opaque_pipeline->name.assign(300U, 'x');
    request.directional_shadows = invalid_shadows;
    auto invalid_shadow_pipeline = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(!invalid_shadow_pipeline.ok() &&
                invalid_shadow_pipeline.status ==
                    apex::app::WorkspaceViewportStatus::invalid &&
                invalid_shadow_pipeline.diagnostic.code ==
                    "workspace_viewport_shadow_pipeline_name_limit" &&
                device.texture_calls == 0U && device.depth_calls == 0U,
            "over-limit shadow program fails before backend allocation");

    invalid_shadows = {};
    invalid_shadows.alpha_static_pipeline = alpha_shadow_pipeline(value);
    invalid_shadows.alpha_static_pipeline->shaders.pop_back();
    request.directional_shadows = invalid_shadows;
    auto incomplete_alpha_shadow = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(!incomplete_alpha_shadow.ok() &&
                incomplete_alpha_shadow.diagnostic.code ==
                    "workspace_viewport_shadow_depth_only_indexed_shader_pair_invalid" &&
                device.texture_calls == 0U && device.depth_calls == 0U,
            "incomplete alpha shadow program fails before map allocation");

    invalid_shadows = {};
    invalid_shadows.skinned_pipeline = skinned_shadow_pipeline(value);
    invalid_shadows.skinned_pipeline->vertex_layout.stride =
        11U * sizeof(float);
    invalid_shadows.skinned_pipeline->vertex_layout.attributes.resize(4U);
    request.directional_shadows = invalid_shadows;
    auto malformed_skinned_shadow =
        apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(!malformed_skinned_shadow.ok() &&
                malformed_skinned_shadow.diagnostic.code ==
                    "workspace_viewport_shadow_depth_only_indexed_pipeline_"
                    "vertex_layout_invalid" &&
                device.texture_calls == 0U && device.depth_calls == 0U,
            "malformed skinned shadow stream fails before map allocation");
    value.module_set.directional_shadow_receiver = false;

    request = request_for(value);
    request.shader_modules = {};
    auto missing_modules =
        apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(!missing_modules.ok() &&
                missing_modules.status ==
                    apex::app::WorkspaceViewportStatus::unsupported &&
                missing_modules.diagnostic.code ==
                    "stock_material_shader_module_missing" &&
                !missing_modules.viewport,
            "missing caller shader modules fail without a viewport");

    auto malformed = value.document;
    malformed.scene.snapshot.root = apex::scene::invalid_node_id;
    auto malformed_result =
        apex::app::prepareWorkspaceViewport(device, malformed, request);
    require(!malformed_result.ok() && malformed_result.diagnostic.code ==
                                          "workspace_viewport_document_invalid",
            "malformed document fails before rendering");

    request = request_for(value);
    request.color_samples = 2U;
    auto unsupported_samples =
        apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(!unsupported_samples.ok() &&
                unsupported_samples.status ==
                    apex::app::WorkspaceViewportStatus::unsupported &&
                unsupported_samples.diagnostic.code ==
                    "workspace_viewport_multisample_unsupported",
            "unsupported multisample count is rejected before allocation");

    request = request_for(value);
    request.limits.max_scene_nodes = 0U;
    auto limited =
        apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(!limited.ok() &&
                limited.status == apex::app::WorkspaceViewportStatus::invalid &&
                !limited.viewport,
            "scene limits fail without a partial viewport");
}

void rejects_frame_mismatch_and_preserves_present_atomicity() {
    auto value = fixture();
    auto request = request_for(value);
    FakeDevice device;
    auto prepared =
        apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(prepared.ok(), "viewport setup for frame rejection succeeds");

    PresentationTargetDescription wrong_description = request.presentation;
    wrong_description.width = 16U;
    FakeTarget wrong_target(wrong_description);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    FakeDevice foreign_device;
    FakeTarget matching_target(request.presentation);
    auto status = prepared.viewport->drawAndPresent(
        foreign_device, matching_target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "workspace_viewport_device_mismatch" &&
                foreign_device.draw_calls == 0U &&
                foreign_device.present_calls == 0U,
            "viewport rejects a different same-backend device");

    status = prepared.viewport->drawAndPresent(device, wrong_target, frame,
                                               diagnostic);
    require(status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "workspace_viewport_target_mismatch" &&
                device.present_calls == 0U,
            "wrong target size fails before present");

    PresentationTargetDescription wrong_format_description =
        request.presentation;
    wrong_format_description.format = TextureFormat::bgra8_unorm;
    FakeTarget wrong_format_target(wrong_format_description);
    status = prepared.viewport->drawAndPresent(device, wrong_format_target,
                                               frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "workspace_viewport_target_mismatch" &&
                device.present_calls == 0U,
            "wrong target format fails before present");

    FakeTarget target(request.presentation);
    device.fail_draw = true;
    status =
        prepared.viewport->drawAndPresent(device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::execution_failed &&
                diagnostic.code == "fake_draw_failed" &&
                device.present_calls == 0U,
            "draw failure never presents a partial frame");

    device.fail_draw = false;
    device.invalid_draw = true;
    status =
        prepared.viewport->drawAndPresent(device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "fake_draw_invalid" &&
                device.present_calls == 0U,
            "invalid draw status is preserved and never presented");

    device.invalid_draw = false;
    device.unsupported_draw = true;
    status =
        prepared.viewport->drawAndPresent(device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::unsupported &&
                diagnostic.code == "fake_draw_unsupported" &&
                device.present_calls == 0U,
            "unsupported draw status is preserved and never presented");

    device.unsupported_draw = false;
    frame.camera.clip_space = CameraClipSpace::d3d12;
    status = prepared.viewport->drawAndPresent(device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "workspace_viewport_camera_clip_space" && device.present_calls == 0U,
            "wrong backend camera convention fails before draw and present");
}

} // namespace

int main() {
    try {
        builds_checked_stock_vulkan_source_frames();
        evaluates_bounded_workspace_lighting();
        materializes_external_texture_before_backend_preparation();
        materializes_solid_color_before_backend_preparation();
        rejects_invalid_solid_color_before_gpu_allocation();
        rejects_invalid_external_textures_before_gpu_allocation();
        opens_and_draws();
        draws_portable_sky_before_main_color_and_requires_constants();
        prepares_and_draws_portable_clouds_with_retained_resources();
        captures_portable_clouds_on_all_reflection_faces();
        rejects_malformed_portable_cloud_options_atomically();
        prepares_draws_and_toggles_portable_grass_with_retained_resources();
        captures_portable_grass_on_all_reflection_faces();
        rejects_malformed_portable_grass_options_atomically();
        draws_explicit_multimap_reflection_cube_outside_model_textures();
        captures_and_publishes_six_portable_reflection_faces_atomically();
        rejects_invalid_portable_reflection_capture_options_before_allocation();
        draws_four_sample_viewport_through_retained_resolve();
        draws_opt_in_hdr_viewport_before_presentation();
        draws_automatic_exposure_before_tone_map();
        draws_opt_in_fxaa_after_hdr_postprocessing();
        reallocates_fxaa_targets_after_viewport_resize();
        rejects_invalid_viewport_hdr_requests();
        draws_builtin_vulkan_source_through_hdr_tone_map();
        prepares_retains_and_toggles_recovered_skeleton_overlay();
        rejects_malformed_skeleton_viewport_inputs_before_allocation();
        draws_selected_axis_inside_the_scene_batch();
        draws_and_toggles_recovered_world_view_axis();
        draws_raw_ai_spline_in_recovered_scene_phase();
        draws_recovered_ai_spline_side_passes();
        draws_recovered_ai_spline_selected_index_pass();
        draws_recovered_ai_spline_camber_pass();
        replaces_committed_ai_spline_overlays_atomically();
        tracks_recovered_ai_spline_manual_input();
        routes_portable_ai_spline_side_visibility_commands();
        publishes_ai_spline_side_visibility_atomically();
        publishes_ai_spline_controller_transactions();
        publishes_recovered_ai_spline_edit_lifecycle();
        publishes_temporary_ai_spline_edit_transaction();
        publishes_recovered_ai_spline_point_selection();
        rejects_foreign_ai_spline_controller_generations();
        normalizes_legacy_and_rejects_unsafe_ai_spline_controller_candidates();
        handles_degenerate_ai_spline_manual_forwards();
        publishes_controller_through_d3d12_metadata_contract();
        draws_selected_mesh_with_recovered_fade_boundary();
        toggles_prepared_authoring_grid_per_frame();
        rejects_unbound_selection_axis_requests();
        accepts_track_and_car_lod_documents();
        selects_car_lod_roots_at_viewport_boundary();
        applies_live_camera_mesh_filter_without_resource_rebuild();
        updates_webgl_compatible_transparent_order_per_frame();
        combines_workspace_lod_and_live_mesh_visibility();
        resolves_preview_state_without_mutating_document();
        rejects_staged_fbx_before_backend_allocation();
        camera_controller_matches_bounded_editor_gestures();
        camera_controller_supports_keyboard_translation();
        schedules_directional_shadows_before_color_and_reuses_maps();
        prepares_and_draws_builtin_vulkan_source_viewport();
        rejects_invalid_builtin_vulkan_source_frames_before_draw();
        preserves_portable_and_d3d12_viewport_paths();
        rejects_invalid_d3d12_native_viewport_selection_before_allocation();
        shares_live_camera_visibility_with_directional_shadows();
        retains_independent_shadowgen_visibility();
        rejects_invalid_inputs();
        rejects_frame_mismatch_and_preserves_present_atomicity();
        std::cout << "workspace_viewport_tests: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "workspace_viewport_tests: " << error.what() << '\n';
        return 1;
    }
}
