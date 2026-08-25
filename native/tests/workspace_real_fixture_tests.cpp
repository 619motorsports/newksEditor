#include "apex/app/workspace_selection.hpp"
#include "apex/app/workspace_session.hpp"
#include "apex/app/workspace_viewport.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace apex::render;

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
        ++buffers;
        return {BufferStatus::ready, {}, std::make_unique<FakeBuffer>(description)};
    }

    BufferUpdateResult update_buffer(Buffer&, std::uint64_t,
                                     std::span<const std::byte>) override {
        return {BufferStatus::ready, {}};
    }

    TextureResult create_texture(const TextureDescription& description,
                                 const TextureUploadPlan&) override {
        ++textures;
        return {TextureStatus::ready, {}, std::make_unique<FakeTexture>(description)};
    }

    TextureUpdateResult update_texture(Texture&, const TextureUploadPlan&) override {
        return {TextureStatus::ready, {}};
    }

    DepthAttachmentResult create_depth_attachment(
        const DepthAttachmentDescription& description) override {
        ++depth_attachments;
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
        ++draws;
        return {IndexedStaticMeshBatchStatus::ready, {}, {}};
    }

    SamplerResult create_sampler(const SamplerDescription& description) override {
        ++samplers;
        return {SamplerStatus::ready, {}, std::make_unique<FakeSampler>(description)};
    }

    ShaderModuleResult create_shader_module(const ShaderModuleDescription&) override {
        return {ShaderModuleStatus::unsupported, {"unused", "unused"}, nullptr};
    }

    PresentationFrameResult present_texture(PresentationTarget&, Texture&) override {
        ++presents;
        return {PresentationFrameStatus::ready, {}};
    }

    void wait_idle() noexcept override {}

    std::size_t buffers = 0U;
    std::size_t textures = 0U;
    std::size_t depth_attachments = 0U;
    std::size_t samplers = 0U;
    std::size_t draws = 0U;
    std::size_t presents = 0U;

private:
    DeviceInfo info_{Backend::Vulkan, "workspace composition fake", "unit", 1U, 0U,
                     0U, 0U, 0U, true};
};

std::vector<std::uint8_t> read_fixture() {
    constexpr std::array<const char*, 5U> paths = {
        "test/content/cars/619_gen6_arca_base/619_gen6_fusion13.kn5",
        "../test/content/cars/619_gen6_arca_base/619_gen6_fusion13.kn5",
        "../../test/content/cars/619_gen6_arca_base/619_gen6_fusion13.kn5",
        "../../../test/content/cars/619_gen6_arca_base/619_gen6_fusion13.kn5",
        "../../../../test/content/cars/619_gen6_arca_base/619_gen6_fusion13.kn5"};
    for (const char* path : paths) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) continue;
        const auto end = input.tellg();
        if (end <= 0) continue;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
        input.seekg(0, std::ios::beg);
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (input.good() || input.eof()) return bytes;
    }
    return {};
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
            bytes[index * sizeof(std::uint32_t) + byte] = static_cast<std::uint8_t>(
                words[index] >> (byte * 8U));
    return bytes;
}

void opens_real_model_and_presents_through_the_composition_seams(
    std::span<const std::uint8_t> bytes) {
    apex::app::WorkspaceSessionFile file;
    file.name = "619_gen6_fusion13.kn5";
    file.bytes = bytes;
    apex::app::WorkspaceSessionOpenRequest open_request;
    open_request.name = file.name;
    open_request.modelFiles = std::span<const apex::app::WorkspaceSessionFile>(&file, 1U);
    const auto opened = apex::app::WorkspaceSession{}.open(open_request);
    require(opened.ok(), "real KN5 fixture opens through the application session");
    require(opened.document->assembly.model.materials.size() == 39U,
            "real LFS fixture material metadata reaches the application document");
    require(opened.document->assembly.model.textures.size() == 63U,
            "real LFS fixture texture metadata reaches the application document");
    require(opened.document->scene.snapshot.nodes.size() > 0U,
            "real LFS fixture node metadata reaches the application document");

    std::set<std::string> families;
    for (const auto& material : opened.document->assembly.model.materials)
        families.insert(material.shader);
    std::vector<std::array<PipelineShaderModule, 2U>> module_storage;
    std::vector<StockMaterialShaderModules> descriptors;
    module_storage.reserve(families.size());
    descriptors.reserve(families.size());
    const auto bytes_for_shader = shader_bytes();
    for (const auto& family : families) {
        module_storage.push_back({
            PipelineShaderModule{PipelineShaderStage::vertex,
                                 PipelineShaderFormat::spirv, bytes_for_shader},
            PipelineShaderModule{PipelineShaderStage::fragment,
                                 PipelineShaderFormat::spirv, bytes_for_shader}});
        descriptors.push_back({StockMaterialShaderKeyKind::shader_family, family,
                               std::span<const PipelineShaderModule>(
                                   module_storage.back().data(), 2U)});
    }

    FakeDevice device;
    apex::app::WorkspaceViewportPrepareRequest prepare_request;
    prepare_request.presentation.width = 64U;
    prepare_request.presentation.height = 64U;
    prepare_request.presentation.format = TextureFormat::rgba8_unorm;
    prepare_request.shader_modules = descriptors;
    std::vector<MaterialBindingOverrides> overrides(
        opened.document->assembly.model.materials.size());
    for (auto& override_value : overrides) {
        override_value.properties.emplace(
            "fresnelMaxLevel", MaterialPropertyOverride::scalar_value(0.0F));
        override_value.properties.emplace(
            "useDetail", MaterialPropertyOverride::scalar_value(0.0F));
    }
    prepare_request.overrides_by_material = overrides;
    prepare_request.render.camera_position = {0.0F, 0.0F, 5.0F};
    prepare_request.render.include_reflections = false;
    prepare_request.render.include_shadows = false;
    const auto isolated = std::find_if(
        opened.document->scene.snapshot.nodes.begin(),
        opened.document->scene.snapshot.nodes.end(),
        [&](const auto& node) {
            return node.renderable && node.material != apex::scene::invalid_material_id &&
                   static_cast<std::size_t>(node.material) <
                       opened.document->assembly.model.materials.size() &&
                   opened.document->assembly.model.materials[node.material].shader ==
                       "ksPerPixel";
        });
    require(isolated != opened.document->scene.snapshot.nodes.end(),
            "real model contains an isolated renderable node");
    apex::app::WorkspaceSelectionRequest selection_request;
    selection_request.collect_matches = false;
    selection_request.selected_node = isolated->id;
    selection_request.isolate_selected = true;
    selection_request.show_hidden = true;
    selection_request.wireframe = true;
    const auto selection = apex::app::resolve_workspace_selection(
        opened.document->scene.snapshot, selection_request);
    require(selection.ok(),
            "real model selection resolves through the bounded hierarchy service");
    prepare_request.render.isolated = selection.state.isolate_selected;
    prepare_request.render.isolated_node = selection.state.selected_node;
    prepare_request.render.show_hidden = selection.state.show_hidden;
    prepare_request.packets.selected_node = selection.state.selected_node;
    prepare_request.packets.wireframe = selection.state.wireframe;
    prepare_request.wireframe = selection.state.wireframe;
    const auto prepared = apex::app::prepareWorkspaceViewport(
        device, *opened.document, prepare_request);
    if (!prepared.ok())
        std::cerr << "workspace_real_fixture_tests: prepare diagnostic "
                  << prepared.diagnostic.code << ": "
                  << prepared.diagnostic.message << '\n';
    require(prepared.ok(), "real model reaches viewport preparation with module contracts");
    require(!prepared.viewport->renderPlan().items.empty() && device.textures > 0U &&
                device.depth_attachments == 1U,
            "viewport preparation owns real model resources");

    FakeTarget target(prepare_request.presentation);
    apex::app::WorkspaceViewportFrameRequest frame;
    frame.camera.clip_space = CameraClipSpace::vulkan;
    frame.frame_constants = KsPerPixelFrameConstants{};
    Diagnostic diagnostic;
    const auto status = prepared.viewport->drawAndPresent(
        device, target, frame, diagnostic);
    require(status == apex::app::WorkspaceViewportFrameStatus::ready &&
                device.draws > 0U && device.presents == 1U,
            "real model draws and presents through the backend-neutral seam");
}

void rejects_truncated_real_model_atomically(std::span<const std::uint8_t> bytes) {
    require(bytes.size() > 1U, "real fixture is large enough for truncation coverage");
    apex::app::WorkspaceSessionFile file;
    file.name = "619_gen6_fusion13.truncated.kn5";
    file.bytes = bytes.first(bytes.size() - 1U);
    apex::app::WorkspaceSessionOpenRequest request;
    request.name = file.name;
    request.modelFiles = std::span<const apex::app::WorkspaceSessionFile>(&file, 1U);
    const auto result = apex::app::WorkspaceSession{}.open(request);
    require(!result.ok() && !result.document.has_value() &&
                !result.diagnostics.empty() &&
                result.diagnostics.front().code == "MODEL_INVALID" &&
                result.diagnostics.front().path == file.name,
            "truncated real LFS model fails atomically with source attribution");
}

}  // namespace

int main() {
    const auto bytes = read_fixture();
    if (bytes.empty()) {
        std::cerr << "workspace_real_fixture_tests: repository LFS fixture unavailable\n";
        return 77;
    }
    try {
        rejects_truncated_real_model_atomically(bytes);
        opens_real_model_and_presents_through_the_composition_seams(bytes);
        std::cout << "workspace_real_fixture_tests: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "workspace_real_fixture_tests: " << error.what() << '\n';
        return 1;
    }
}
