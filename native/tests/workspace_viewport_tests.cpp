#include "apex/app/workspace_viewport.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
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
    const DeviceInfo& info() const noexcept override { return info_; }

    BufferResult create_buffer(const BufferDescription& description,
                               std::span<const std::byte>) override {
        ++buffer_calls;
        return {BufferStatus::ready, {}, std::make_unique<FakeBuffer>(description)};
    }

    BufferUpdateResult update_buffer(Buffer&, std::uint64_t,
                                     std::span<const std::byte>) override {
        return {BufferStatus::ready, {}};
    }

    TextureResult create_texture(const TextureDescription& description,
                                 const TextureUploadPlan&) override {
        ++texture_calls;
        return {TextureStatus::ready, {}, std::make_unique<FakeTexture>(description)};
    }

    TextureUpdateResult update_texture(Texture&, const TextureUploadPlan&) override {
        return {TextureStatus::ready, {}};
    }

    DepthAttachmentResult create_depth_attachment(
        const DepthAttachmentDescription& description) override {
        ++depth_calls;
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
        Texture&, const IndexedStaticMeshBatchDescription& batch) override {
        ++draw_calls;
        events.push_back("color");
        draw_counts.push_back(batch.draws.size());
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
        PresentationTarget&, Texture&) override {
        ++present_calls;
        events.push_back("present");
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
    std::vector<std::vector<apex::scene::NodeId>> draw_nodes;
    std::vector<std::string> events;
    std::vector<const DepthAttachment*> depth_targets;
    std::vector<std::size_t> depth_draw_counts;
    std::vector<float> depth_clear_values;
    std::vector<std::array<const DepthAttachment*,
                           indexed_directional_shadow_cascade_count>> receiver_maps;

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
    frame.frame_constants = KsPerPixelFrameConstants{};
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
    request.color_samples = 4U;
    auto multisample = apex::app::prepareWorkspaceViewport(device, value.document, request);
    require(!multisample.ok() &&
                multisample.status == apex::app::WorkspaceViewportStatus::unsupported &&
                multisample.diagnostic.code == "workspace_viewport_multisample_unsupported",
            "multisample presentation is rejected explicitly");

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
        opens_and_draws();
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
