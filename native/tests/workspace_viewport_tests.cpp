#include "apex/app/workspace_viewport.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
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
        Texture&, const IndexedStaticMeshBatchDescription&) override {
        ++draw_calls;
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

    SamplerResult create_sampler(const SamplerDescription& description) override {
        return {SamplerStatus::ready, {}, std::make_unique<FakeSampler>(description)};
    }

    ShaderModuleResult create_shader_module(const ShaderModuleDescription&) override {
        return {ShaderModuleStatus::unsupported, {"unused", "unused"}, nullptr};
    }

    PresentationFrameResult present_texture(
        PresentationTarget&, Texture&) override {
        ++present_calls;
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
    std::size_t texture_calls = 0U;
    std::size_t depth_calls = 0U;
    std::size_t draw_calls = 0U;
    std::size_t present_calls = 0U;

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
    Diagnostic diagnostic;
    const auto status = prepared.viewport->drawAndPresent(device, target, frame, diagnostic);
    require(status == WorkspaceViewportFrameStatus::ready &&
                device.draw_calls == 1U && device.present_calls == 1U,
            "viewport draws before presenting one frame");
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
        resolves_preview_state_without_mutating_document();
        rejects_invalid_inputs();
        rejects_frame_mismatch_and_preserves_present_atomicity();
        std::cout << "workspace_viewport_tests: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "workspace_viewport_tests: " << error.what() << '\n';
        return 1;
    }
}
