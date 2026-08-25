#include "apex/app/workspace_viewport.hpp"
#include "apex/app/workspace_shadow_programs.hpp"
#include "apex/render/view_axis.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace apex::render;
using apex::app::WorkspaceViewportFrameRequest;
using apex::app::WorkspaceViewportFrameStatus;
using apex::app::WorkspaceViewportPrepareRequest;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class FakeBuffer final : public Buffer {
public:
    explicit FakeBuffer(BufferDescription description) : info_({description}) {}
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const BufferInfo& info() const noexcept override { return info_; }
private:
    BufferInfo info_{};
};

class FakeTexture final : public Texture {
public:
    explicit FakeTexture(TextureDescription description) : info_({description}) {}
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const TextureInfo& info() const noexcept override { return info_; }
private:
    TextureInfo info_{};
};

class FakeDepth final : public DepthAttachment {
public:
    explicit FakeDepth(DepthAttachmentDescription description) : info_({description}) {}
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const DepthAttachmentInfo& info() const noexcept override { return info_; }
private:
    DepthAttachmentInfo info_{};
};

class FakeSampler final : public Sampler {
public:
    explicit FakeSampler(SamplerDescription description) : info_({description}) {}
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const SamplerInfo& info() const noexcept override { return info_; }
private:
    SamplerInfo info_{};
};

class FakeTarget final : public PresentationTarget {
public:
    explicit FakeTarget(PresentationTargetDescription description) : info_({description}) {}
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const PresentationTargetInfo& info() const noexcept override { return info_; }
private:
    PresentationTargetInfo info_{};
};

class FakeDevice final : public Device {
public:
    struct BufferUpdate {
        Buffer* buffer = nullptr;
        std::uint64_t offset = 0U;
        std::vector<std::byte> bytes;
    };

    const DeviceInfo& info() const noexcept override { return info_; }

    BufferResult create_buffer(const BufferDescription& description,
                               std::span<const std::byte>) override {
        ++buffer_calls;
        return {BufferStatus::ready, {}, std::make_unique<FakeBuffer>(description)};
    }

    BufferUpdateResult update_buffer(Buffer& buffer, std::uint64_t offset,
                                     std::span<const std::byte> bytes) override {
        buffer_updates.push_back(
            {&buffer, offset, {bytes.begin(), bytes.end()}});
        return {BufferStatus::ready, {}};
    }

    TextureResult create_texture(const TextureDescription& description,
                                 const TextureUploadPlan&) override {
        ++texture_calls;
        auto texture = std::make_unique<FakeTexture>(description);
        created_texture_descriptions.push_back(description);
        created_textures.push_back(texture.get());
        return {TextureStatus::ready, {}, std::move(texture)};
    }

    TextureUpdateResult update_texture(Texture&, const TextureUploadPlan&) override {
        return {TextureStatus::ready, {}};
    }

    DepthAttachmentResult create_depth_attachment(
        const DepthAttachmentDescription& description) override {
        ++depth_calls;
        created_depth_descriptions.push_back(description);
        return {DepthAttachmentStatus::ready, {}, std::make_unique<FakeDepth>(description)};
    }

    TextureClearReadbackResult clear_texture_and_readback(
        Texture&, const TextureClearReadbackRequest&) override {
        return {TextureReadbackStatus::unsupported, {"unused", "unused"}, {}};
    }

    TriangleDrawResult draw_triangle_and_readback(
        Texture&, const TriangleDrawRequest&) override {
        return {TriangleDrawStatus::unsupported, {"unused", "unused"}, {}};
    }

    IndexedStaticMeshBatchResult draw_indexed_static_mesh_batch_and_readback(
        Texture& texture, const IndexedStaticMeshBatchDescription& batch) override {
        ++draw_calls;
        events.push_back("color");
        draw_targets.push_back(&texture);
        resolve_targets.push_back(batch.resolve_target);
        capture_requests.push_back(batch.capture_rgba8);
        draw_counts.push_back(batch.draws.size());
        overlay_counts.push_back(batch.overlay_draws.size());
        selected_mesh_counts.push_back(batch.selected_mesh_draws.size());
        for (const auto& overlay : batch.overlay_draws) {
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
        for (const auto& draw : batch.draws)
            nodes.push_back(draw.packet == nullptr
                                ? apex::scene::invalid_node_id
                                : draw.packet->node);
        draw_nodes.push_back(std::move(nodes));
        if (!batch.draws.empty()) {
            receiver_maps.push_back(
                batch.draws.front().directional_shadow_binding.maps);
        }
        if (fail_draw)
            return {IndexedStaticMeshBatchStatus::execution_failed,
                    {"fake_draw_failed", "injected draw failure"}, {}};
        if (invalid_draw)
            return {IndexedStaticMeshBatchStatus::invalid_request,
                    {"fake_draw_invalid", "injected invalid draw"}, {}};
        if (unsupported_draw)
            return {IndexedStaticMeshBatchStatus::unsupported,
                    {"fake_draw_unsupported", "injected unsupported draw"}, {}};
        return {IndexedStaticMeshBatchStatus::ready, {}, {}};
    }

    DepthOnlyIndexedStaticMeshBatchResult draw_depth_only_indexed_static_mesh_batch(
        const DepthOnlyIndexedStaticMeshBatchDescription& batch) override {
        ++depth_batch_calls;
        events.push_back("shadow");
        depth_targets.push_back(batch.depth_attachment);
        depth_draw_counts.push_back(batch.draws.size());
        depth_clear_values.push_back(batch.clear_depth ? batch.depth_clear_value
                                                       : -1.0F);
        if (!batch.draws.empty() && batch.draws.front().camera_frame.has_value()) {
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

    SamplerResult create_sampler(const SamplerDescription& description) override {
        ++sampler_calls;
        return {SamplerStatus::ready, {}, std::make_unique<FakeSampler>(description)};
    }

    ShaderModuleResult create_shader_module(const ShaderModuleDescription&) override {
        return {ShaderModuleStatus::unsupported, {"unused", "unused"}, nullptr};
    }

    PresentationFrameResult present_texture(
        PresentationTarget&, Texture& texture) override {
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
    bool invalid_draw = false;
    bool unsupported_draw = false;
    bool fail_present = false;
    std::size_t fail_depth_batch_call = 0U;
    std::size_t buffer_calls = 0U;
    std::size_t texture_calls = 0U;
    std::size_t depth_calls = 0U;
    std::size_t sampler_calls = 0U;
    std::size_t draw_calls = 0U;
    std::size_t depth_batch_calls = 0U;
    std::size_t present_calls = 0U;
    std::vector<std::size_t> draw_counts;
    std::vector<std::size_t> overlay_counts;
    std::vector<std::size_t> selected_mesh_counts;
    std::vector<DrawMatrices> overlay_matrices;
    std::vector<const Buffer*> overlay_buffers;
    std::vector<std::uint32_t> overlay_scene_positions;
    std::vector<std::uint64_t> overlay_vertex_offsets;
    std::vector<std::uint32_t> overlay_vertex_counts;
    std::vector<bool> overlay_depth_tests;
    std::vector<bool> overlay_depth_writes;
    std::vector<std::vector<apex::scene::NodeId>> draw_nodes;
    std::vector<std::string> events;
    std::vector<const DepthAttachment*> depth_targets;
    std::vector<std::size_t> depth_draw_counts;
    std::vector<float> depth_clear_values;
    std::vector<apex::scene::Matrix4> depth_camera_matrices;
    std::vector<BufferUpdate> buffer_updates;
    std::vector<std::array<const DepthAttachment*,
                           indexed_directional_shadow_cascade_count>> receiver_maps;
    std::vector<TextureDescription> created_texture_descriptions;
    std::vector<Texture*> created_textures;
    std::vector<DepthAttachmentDescription> created_depth_descriptions;
    std::vector<Texture*> resolve_targets;
    std::vector<bool> capture_requests;
    std::vector<Texture*> presented_textures;
    std::vector<Texture*> draw_targets;

private:
    DeviceInfo info_{Backend::Vulkan, "viewport fake", "unit", 1U, 0U, 0U,
                     0U, 0U, true};
};

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset,
           std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
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

CameraFrame valid_shadow_camera(float x = 0.0F) {
    CameraFrameRequest request;
    request.eye = {x, 0.0F, 5.0F};
    request.target = {0.0F, 0.0F, 0.0F};
    request.aspect = 1.0F;
    request.near_plane = 0.01F;
    request.far_plane = 100.0F;
    request.clip_space = CameraClipSpace::vulkan;
    const auto built = build_camera_frame(request);
    require(built.ok(), "directional shadow test camera builds");
    return *built.frame;
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

void draws_selected_axis_inside_the_scene_batch() {
    auto value = fixture();
    auto request = request_for(value);
    request.packets.selected_node = 1U;
    request.authoring_overlay_pipeline = authoring_overlay_pipeline(value);
    request.grid_visible = true;
    value.document.scene.snapshot.nodes[1U].transform[12] = 2.0F;
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
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
    require(!wrong_height.ok() && wrong_height.diagnostic.code ==
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
    require(!incomplete_side.ok() && incomplete_side.diagnostic.code ==
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
    require(!wrong_topology.ok() && wrong_topology.diagnostic.code ==
                "workspace_viewport_ai_spline_geometry_invalid",
            "selected-index markers reject polyline topology");

    auto wrong_state_geometry = selection.geometry;
    wrong_state_geometry.last_selected_index =
        wrong_state_geometry.source_point_count;
    auto wrong_state_request = request;
    wrong_state_request.ai_spline_selection_geometry =
        &wrong_state_geometry;
    FakeDevice wrong_state_device;
    const auto wrong_state = apex::app::prepareWorkspaceViewport(
        wrong_state_device, value.document, wrong_state_request);
    require(!wrong_state.ok() && wrong_state.diagnostic.code ==
                "workspace_viewport_ai_spline_geometry_invalid",
            "selected-index markers reject an invalid last-selected index");
}

void draws_selected_mesh_with_recovered_fade_boundary() {
    auto value = fixture();
    auto request = request_for(value);
    request.packets.selected_node = 1U;
    request.selected_mesh_pipeline = selected_mesh_pipeline(value);
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(prepared.ok(), "selected-mesh viewport preparation succeeds");

    FakeTarget target(request.presentation);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    frame.selected_mesh_elapsed_ms = 0U;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready,
            "selected-mesh initial frame draws");
    frame.selected_mesh_elapsed_ms = 2000U;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready,
            "selected-mesh zero-alpha boundary frame draws");
    frame.selected_mesh_elapsed_ms = 2001U;
    require(prepared.viewport->drawAndPresent(device, target, frame, diagnostic) ==
                WorkspaceViewportFrameStatus::ready,
            "selected-mesh expired frame draws scene only");
    require(device.selected_mesh_counts ==
                std::vector<std::size_t>({1U, 1U, 0U}),
            "selected mesh remains scheduled at 2000 ms and expires after it");

    std::vector<std::array<float, 4U>> colors;
    for (const auto& update : device.buffer_updates) {
        if (update.bytes.size() != sizeof(std::array<float, 4U>)) continue;
        std::array<float, 4U> color{};
        std::memcpy(color.data(), update.bytes.data(), update.bytes.size());
        colors.push_back(color);
    }
    require(colors.size() == 2U &&
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
    invalid_shadows.skinned_pipeline->vertex_layout.stride = 11U * sizeof(float);
    invalid_shadows.skinned_pipeline->vertex_layout.attributes.resize(4U);
    request.directional_shadows = invalid_shadows;
    auto malformed_skinned_shadow = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(!malformed_skinned_shadow.ok() &&
                malformed_skinned_shadow.diagnostic.code ==
                    "workspace_viewport_shadow_depth_only_indexed_pipeline_vertex_layout_invalid" &&
                device.texture_calls == 0U && device.depth_calls == 0U,
            "malformed skinned shadow stream fails before map allocation");
    value.module_set.directional_shadow_receiver = false;

    request = request_for(value);
    request.shader_modules = {};
    auto missing_modules = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(!missing_modules.ok() &&
                missing_modules.status == apex::app::WorkspaceViewportStatus::unsupported &&
                missing_modules.diagnostic.code == "stock_material_shader_module_missing" &&
                !missing_modules.viewport,
            "missing caller shader modules fail without a viewport");

    auto malformed = value.document;
    malformed.scene.snapshot.root = apex::scene::invalid_node_id;
    auto malformed_result = apex::app::prepareWorkspaceViewport(device, malformed, request);
    require(!malformed_result.ok() &&
                malformed_result.diagnostic.code == "workspace_viewport_document_invalid",
            "malformed document fails before rendering");

    request = request_for(value);
    request.color_samples = 2U;
    auto unsupported_samples = apex::app::prepareWorkspaceViewport(
        device, value.document, request);
    require(!unsupported_samples.ok() &&
                unsupported_samples.status ==
                    apex::app::WorkspaceViewportStatus::unsupported &&
                unsupported_samples.diagnostic.code ==
                    "workspace_viewport_multisample_unsupported",
            "unsupported multisample count is rejected before allocation");

    request = request_for(value);
    request.limits.max_scene_nodes = 0U;
    auto limited = apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(!limited.ok() &&
                limited.status == apex::app::WorkspaceViewportStatus::invalid &&
                !limited.viewport,
            "scene limits fail without a partial viewport");
}

void rejects_frame_mismatch_and_preserves_present_atomicity() {
    auto value = fixture();
    auto request = request_for(value);
    FakeDevice device;
    auto prepared = apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(prepared.ok(), "viewport setup for frame rejection succeeds");

    PresentationTargetDescription wrong_description = request.presentation;
    wrong_description.width = 16U;
    FakeTarget wrong_target(wrong_description);
    WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    auto status = prepared.viewport->drawAndPresent(device, wrong_target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "workspace_viewport_target_mismatch" &&
                device.present_calls == 0U,
            "wrong target size fails before present");

    PresentationTargetDescription wrong_format_description = request.presentation;
    wrong_format_description.format = TextureFormat::bgra8_unorm;
    FakeTarget wrong_format_target(wrong_format_description);
    status = prepared.viewport->drawAndPresent(device, wrong_format_target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "workspace_viewport_target_mismatch" &&
                device.present_calls == 0U,
            "wrong target format fails before present");

    FakeTarget target(request.presentation);
    device.fail_draw = true;
    status = prepared.viewport->drawAndPresent(device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::execution_failed &&
                diagnostic.code == "fake_draw_failed" && device.present_calls == 0U,
            "draw failure never presents a partial frame");

    device.fail_draw = false;
    device.invalid_draw = true;
    status = prepared.viewport->drawAndPresent(device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "fake_draw_invalid" && device.present_calls == 0U,
            "invalid draw status is preserved and never presented");

    device.invalid_draw = false;
    device.unsupported_draw = true;
    status = prepared.viewport->drawAndPresent(device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::unsupported &&
                diagnostic.code == "fake_draw_unsupported" && device.present_calls == 0U,
            "unsupported draw status is preserved and never presented");

    device.unsupported_draw = false;
    frame.camera.clip_space = CameraClipSpace::d3d12;
    status = prepared.viewport->drawAndPresent(device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::invalid &&
                diagnostic.code == "workspace_viewport_camera_clip_space" &&
                device.present_calls == 0U,
            "wrong backend camera convention fails before draw and present");
}

}  // namespace

int main() {
    try {
        evaluates_bounded_workspace_lighting();
        opens_and_draws();
        draws_four_sample_viewport_through_retained_resolve();
        draws_selected_axis_inside_the_scene_batch();
        draws_and_toggles_recovered_world_view_axis();
        draws_raw_ai_spline_in_recovered_scene_phase();
        draws_recovered_ai_spline_side_passes();
        draws_recovered_ai_spline_selected_index_pass();
        draws_recovered_ai_spline_camber_pass();
        draws_selected_mesh_with_recovered_fade_boundary();
        toggles_prepared_authoring_grid_per_frame();
        rejects_unbound_selection_axis_requests();
        accepts_track_and_car_lod_documents();
        selects_car_lod_roots_at_viewport_boundary();
        resolves_preview_state_without_mutating_document();
        camera_controller_matches_bounded_editor_gestures();
        camera_controller_supports_keyboard_translation();
        schedules_directional_shadows_before_color_and_reuses_maps();
        rejects_invalid_inputs();
        rejects_frame_mismatch_and_preserves_present_atomicity();
        std::cout << "workspace_viewport_tests: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "workspace_viewport_tests: " << error.what() << '\n';
        return 1;
    }
}
