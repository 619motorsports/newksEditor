#include "apex/render/stock_material_execution.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace apex::render;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class FakeBuffer final : public Buffer {
public:
    FakeBuffer(BufferDescription description, Backend backend)
        : info_{{description}}, backend_(backend) {}
    Backend backend() const noexcept override { return backend_; }
    const BufferInfo& info() const noexcept override { return info_; }
private:
    BufferInfo info_{};
    Backend backend_ = Backend::Vulkan;
};

class FakeTexture final : public Texture {
public:
    FakeTexture(TextureDescription description, Backend backend)
        : info_{{description}}, backend_(backend) {}
    Backend backend() const noexcept override { return backend_; }
    const TextureInfo& info() const noexcept override { return info_; }
private:
    TextureInfo info_{};
    Backend backend_ = Backend::Vulkan;
};

class FakeSampler final : public Sampler {
public:
    FakeSampler(SamplerDescription description, Backend backend)
        : info_{{description}}, backend_(backend) {}
    Backend backend() const noexcept override { return backend_; }
    const SamplerInfo& info() const noexcept override { return info_; }
private:
    SamplerInfo info_{};
    Backend backend_ = Backend::Vulkan;
};

class FakeDevice final : public Device {
public:
    explicit FakeDevice(Backend backend = Backend::Vulkan)
        : info_{backend, "fake", "unit", 1U, 0U, 0U, 0U, 0U, true} {}
    const DeviceInfo& info() const noexcept override { return info_; }
    BufferResult create_buffer(const BufferDescription& description,
                               std::span<const std::byte> initial_data) override {
        ++buffer_calls;
        initial_buffers.emplace_back(initial_data.begin(), initial_data.end());
        return {BufferStatus::ready, {},
                std::make_unique<FakeBuffer>(description, info_.backend)};
    }
    BufferUpdateResult update_buffer(Buffer&, std::uint64_t,
                                     std::span<const std::byte>) override {
        return {BufferStatus::ready, {}};
    }
    TextureResult create_texture(const TextureDescription& description,
                                 const TextureUploadPlan&) override {
        return {TextureStatus::ready, {},
                std::make_unique<FakeTexture>(description, info_.backend)};
    }
    TextureUpdateResult update_texture(Texture&, const TextureUploadPlan&) override {
        return {TextureStatus::ready, {}};
    }
    TextureClearReadbackResult clear_texture_and_readback(
        Texture&, const TextureClearReadbackRequest&) override {
        return {TextureReadbackStatus::unsupported, {"unused", "unused"}, {}};
    }
    TriangleDrawResult draw_triangle_and_readback(Texture&, const TriangleDrawRequest&) override {
        return {TriangleDrawStatus::unsupported, {"unused", "unused"}, {}};
    }
    SamplerResult create_sampler(const SamplerDescription& description) override {
        return {SamplerStatus::ready, {},
                std::make_unique<FakeSampler>(description, info_.backend)};
    }
    ShaderModuleResult create_shader_module(const ShaderModuleDescription&) override {
        return {ShaderModuleStatus::unsupported, {"unused", "unused"}, nullptr};
    }
    void wait_idle() noexcept override {}

    std::size_t buffer_calls = 0U;
    std::vector<std::vector<std::byte>> initial_buffers;
private:
    DeviceInfo info_{};
};

std::vector<std::uint8_t> spirv_fixture() {
    constexpr std::array<std::uint32_t, 29> words = {
        0x07230203U, 0x00010000U, 0x00070000U, 0x00000005U, 0x00000000U,
        0x00020011U, 0x00000001U, 0x0003000eU, 0x00000000U, 0x00000001U,
        0x0005000fU, 0x00000000U, 0x00000001U, 0x6e69616dU, 0x00000000U,
        0x00020013U, 0x00000002U, 0x00030021U, 0x00000003U, 0x00000002U,
        0x00050036U, 0x00000002U, 0x00000001U, 0x00000000U, 0x00000003U,
        0x000200f8U, 0x00000004U, 0x000100fdU, 0x00010038U,
    };
    std::vector<std::uint8_t> result(words.size() * sizeof(std::uint32_t));
    for (std::size_t index = 0U; index < words.size(); ++index)
        for (std::size_t byte = 0U; byte < sizeof(std::uint32_t); ++byte)
            result[index * 4U + byte] = static_cast<std::uint8_t>(words[index] >> (byte * 8U));
    return result;
}

std::vector<std::uint8_t> minimal_spirv_fixture() {
    return {0x03U, 0x02U, 0x23U, 0x07U, 0x00U, 0x00U, 0x01U,
            0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U,
            0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};
}

std::vector<std::uint8_t> dxbc_fixture(std::size_t storage_bytes = 44U) {
    require(storage_bytes >= 44U, "DXBC fixture storage is large enough");
    std::vector<std::uint8_t> result(storage_bytes, 0U);
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t byte = 0U; byte < sizeof(value); ++byte)
            result[offset + byte] =
                static_cast<std::uint8_t>(value >> (byte * 8U));
    };
    put(0U, 0x43425844U);  // DXBC
    put(20U, 1U);
    put(24U, static_cast<std::uint32_t>(storage_bytes));
    put(28U, 1U);
    put(32U, 36U);
    put(36U, 0x58454853U);  // SHEX
    put(40U, 0U);
    return result;
}

std::vector<std::uint8_t> shader_fixture(Backend backend, bool large) {
    if (backend == Backend::Vulkan)
        return large ? spirv_fixture() : minimal_spirv_fixture();
    return dxbc_fixture(large ? 80U : 44U);
}

struct Fixture {
    apex::formats::Kn5File model;
    apex::scene::SceneSnapshot scene;
    std::vector<DrawPacket> packets;
    std::vector<PipelineShaderModule> modules;
    StockMaterialShaderModules module_set;
};

Fixture fixture(std::string shader, Backend backend = Backend::Vulkan) {
    Fixture result;
    const bool skinned = shader == "ksSkinnedMesh";
    result.model.materials.resize(1U);
    auto& material = result.model.materials.front();
    material.name = "seat_material";
    material.shader = std::move(shader);
    material.serializedBlendMode =
        material.shader == "ksPerPixelMultiMap_AT" ||
                material.shader == "ksPerPixelMultiMap_AT_NMDetail"
            ? 2U
            : 0U;
    material.properties.push_back({"fresnelMaxLevel", 0.0F, {}, {}, {}});
    if (material.serializedBlendMode != 0U)
        material.properties.push_back({"ksAlphaRef", 0.5F, {}, {}, {}});
    const std::array<const char*, 5> slots = {"txDiffuse", "txNormal", "txMaps", "txDetail", "txNormalDetail"};
    for (std::size_t index = 0U; index < slots.size(); ++index) {
        material.resources.push_back({slots[index], static_cast<std::uint32_t>(index),
                                      std::string("texture_") + std::to_string(index)});
        result.model.textures.push_back({true, std::string("texture_") + std::to_string(index), 4U, {}, std::nullopt});
    }
    result.model.root.type = 1U;
    result.model.root.kind = "node";
    result.model.root.name = "ROOT";
    apex::formats::Kn5Node mesh;
    mesh.type = skinned ? 3U : 2U;
    mesh.kind = skinned ? "skinnedMesh" : "mesh";
    mesh.name = "SEAT";
    mesh.vertexStride = skinned ? 19U : 11U;
    mesh.materialId = 0U;
    mesh.vertices.resize(3U * mesh.vertexStride, 0.0F);
    mesh.vertices[0] = -0.5F;
    mesh.vertices[mesh.vertexStride] = 0.5F;
    mesh.vertices[mesh.vertexStride * 2U] = 0.5F;
    if (skinned) {
        mesh.bones.push_back({"Bone", apex::scene::identity_matrix});
        for (std::size_t vertex = 0U; vertex < 3U; ++vertex)
            mesh.vertices[vertex * mesh.vertexStride + 11U] = 1.0F;
    }
    mesh.indices = {0U, 1U, 2U};
    result.model.root.children.push_back(std::move(mesh));
    (void)result.scene.add_material({"seat_material", result.model.materials.front().shader,
                                     apex::scene::BlendMode::opaque});
    apex::scene::SceneNode root;
    root.name = "ROOT";
    const auto root_id = result.scene.add_node(std::move(root));
    apex::scene::SceneNode node;
    node.name = "SEAT";
    node.kind = skinned ? apex::scene::NodeKind::skinned_mesh
                        : apex::scene::NodeKind::mesh;
    node.material = 0U;
    const auto node_id = result.scene.add_node(std::move(node), root_id);
    DrawPacket packet;
    packet.node = node_id;
    packet.material = 0U;
    packet.primitive = skinned ? DrawPrimitiveKind::skinned_mesh
                               : DrawPrimitiveKind::static_mesh;
    packet.vertex_count = 3U;
    packet.index_count = 3U;
    packet.vertex_stride_floats = skinned ? 19U : 11U;
    if (skinned) packet.bone_palette.push_back(apex::scene::identity_matrix);
    packet.flags.wireframe = false;
    packet.resources.reserve(slots.size());
    const bool base_multimap = material.shader == "ksPerPixelMultiMap" ||
                               material.shader == "ksPerPixelMultiMap_AT" ||
                               material.shader == "ksPerPixelMultiMapSimpleRefl";
    const std::size_t count = material.shader == "ksPerPixel" || skinned
                                  ? 1U
                                  : material.shader == "ksPerPixelNM"
                                        ? 2U
                                        : base_multimap ? 3U : 5U;
    for (std::size_t index = 0U; index < count; ++index)
        packet.resources.push_back({slots[index], static_cast<std::uint32_t>(index),
                                    static_cast<std::uint32_t>(index), std::string("texture_") + std::to_string(index)});
    result.packets.push_back(std::move(packet));
    const std::vector<std::uint8_t> bytes = shader_fixture(backend, false);
    const auto format = backend == Backend::Vulkan
                            ? PipelineShaderFormat::spirv
                            : PipelineShaderFormat::dxbc;
    result.modules = {{PipelineShaderStage::vertex, format, bytes},
                      {PipelineShaderStage::fragment, format, bytes}};
    result.module_set = {StockMaterialShaderKeyKind::shader_family, material.shader, result.modules};
    return result;
}

Fixture damage_fixture(float dirt = 0.0F, bool include_dust = true) {
    Fixture result = fixture("ksPerPixelMultiMap_damage_dirt");
    result.module_set.variant = include_dust
                                    ? StockMaterialShaderVariant::damage_dust
                                    : StockMaterialShaderVariant::standard;
    auto& material = result.model.materials.front();
    material.properties.push_back(
        {"damageZones", 0.0F, {}, {}, {1.0F, 0.5F, 0.25F, 0.0F}});
    material.properties.push_back({"dirt", dirt, {}, {}, {}});
    const std::array<const char*, 6> slots = {
        "txDiffuse", "txNormal", "txMaps", "txDamage", "txDamageMask", "txDust"};
    const std::array<std::uint32_t, 6> bind_points = {0U, 1U, 2U, 4U, 21U, 5U};
    const std::size_t count = include_dust ? slots.size() : slots.size() - 1U;
    material.resources.clear();
    result.model.textures.clear();
    result.packets.front().resources.clear();
    for (std::size_t index = 0U; index < count; ++index) {
        const std::string texture = std::string("damage_texture_") +
                                    std::to_string(index);
        material.resources.push_back({slots[index], bind_points[index], texture});
        result.model.textures.push_back({true, texture, 4U, {}, std::nullopt});
        result.packets.front().resources.push_back(
            {slots[index], bind_points[index], static_cast<std::uint32_t>(index), texture});
    }
    return result;
}

StockMaterialExecutionRequest request_for(Fixture& fixture) {
    StockMaterialExecutionRequest request;
    request.model = &fixture.model;
    request.scene = &fixture.scene;
    request.packets = fixture.packets;
    request.shader_modules = std::span<const StockMaterialShaderModules>(&fixture.module_set, 1U);
    request.targets.colors.push_back({PipelineRenderTargetFormat::rgba8_unorm,
                                      fixture.model.materials.front().serializedBlendMode == 2U ? 4U : 1U});
    request.targets.has_depth = true;
    request.targets.depth = {PipelineRenderTargetFormat::depth32_float,
                             fixture.model.materials.front().serializedBlendMode == 2U ? 4U : 1U};
    return request;
}

void test_success_and_a2c() {
    FakeDevice device;
    const PresentationCapabilities default_presentation =
        device.presentation_capabilities();
    require(default_presentation.offscreen &&
                !default_presentation.swapchain_api_available &&
                !default_presentation.native_surface_api_available &&
                !default_presentation.headless_surface_api_available,
            "neutral fake devices report only offscreen capability by default");
    Fixture base_fixture = fixture("ksPerPixelMultiMap");
    const auto base_result =
        prepare_stock_material_execution(device, request_for(base_fixture));
    require(base_result.ok() && base_result.resources->draw_count() == 1U &&
                base_result.resources->owned_stock_shadow_constant_count() == 0U,
            "base MultiMap handoff must retain its three-resource packet");

    Fixture skinned_fixture = fixture("ksSkinnedMesh");
    const auto skinned_result = prepare_stock_material_execution(
        device, request_for(skinned_fixture));
    if (!skinned_result.ok())
        throw std::runtime_error("ksSkinnedMesh handoff failed: " +
                                 skinned_result.diagnostic.code + " " +
                                 skinned_result.diagnostic.message);
    require(skinned_result.ok() &&
                skinned_result.resources->draw_count() == 1U,
            "ksSkinnedMesh accepts the explicit one-texture production handoff");

    Fixture base_at_fixture = fixture("ksPerPixelMultiMap_AT");
    const auto base_at_result =
        prepare_stock_material_execution(device, request_for(base_at_fixture));
    require(base_at_result.ok() && base_at_result.resources->draw_count() == 1U &&
                base_at_result.resources->owned_stock_shadow_constant_count() == 1U,
            "base AT MultiMap handoff retains A2C and its exact shadow material record");
    bool found_shadow_alpha = false;
    for (const auto& bytes : device.initial_buffers) {
        if (bytes.size() < sizeof(StockShadowCasterMaterialConstants)) continue;
        StockShadowCasterMaterialConstants constants{};
        std::memcpy(&constants, bytes.data(), sizeof(constants));
        found_shadow_alpha = found_shadow_alpha ||
                             (constants.lighting ==
                                  std::array<float, 4>{0.35F, 0.8F, 0.2F, 30.0F} &&
                              std::abs(constants.emissive_and_alpha_ref[3] - 0.5F) <
                                  0.0001F);
    }
    require(found_shadow_alpha,
            "stock facade uploads ksAlphaRef at byte offset 28 of the shadow record");

    Fixture damage = damage_fixture();
    const auto damage_result =
        prepare_stock_material_execution(device, request_for(damage));
    require(damage_result.ok() && damage_result.resources->draw_count() == 1U,
            "dirt-zero damage handoff accepts six textures including txDust");
    bool found_damage_constants = false;
    for (const std::vector<std::byte>& bytes : device.initial_buffers) {
        if (bytes.size() < sizeof(KsPerPixelMaterialConstants)) continue;
        KsPerPixelMaterialConstants constants{};
        std::memcpy(&constants, bytes.data(), sizeof(constants));
        found_damage_constants = found_damage_constants ||
                                 constants.damage_zones ==
                                     std::array<float, 4>{1.0F, 0.5F, 0.25F, 0.0F};
    }
    require(found_damage_constants,
            "dirt-zero damage handoff uploads the authored damageZones record");

    Fixture no_dust = damage_fixture(0.0F, false);
    const auto no_dust_result =
        prepare_stock_material_execution(device, request_for(no_dust));
    require(no_dust_result.ok(),
            "dirt-zero damage without txDust retains the legacy 12-resource path");

    Fixture missing_packet_dust = damage_fixture();
    missing_packet_dust.packets.front().resources.pop_back();
    const auto missing_packet_dust_result =
        prepare_stock_material_execution(device, request_for(missing_packet_dust));
    require(missing_packet_dust_result.status == StaticSceneResourceStatus::invalid_request &&
                missing_packet_dust_result.diagnostic.code == "stock_material_resource_layout_mismatch",
            "a packet cannot silently drop material txDust");

    Fixture missing_dust_shader_variant = damage_fixture();
    missing_dust_shader_variant.module_set.variant =
        StockMaterialShaderVariant::standard;
    const auto missing_dust_shader_variant_result = prepare_stock_material_execution(
        device, request_for(missing_dust_shader_variant));
    require(missing_dust_shader_variant_result.status ==
                    StaticSceneResourceStatus::unsupported &&
                missing_dust_shader_variant_result.diagnostic.code ==
                    "stock_material_shader_variant_missing",
            "txDust packets reject shader modules without the damage-dust label");

    Fixture wrong_optional_role = damage_fixture();
    wrong_optional_role.packets.front().resources.back().slot = "txDetail";
    const auto wrong_optional_role_result =
        prepare_stock_material_execution(device, request_for(wrong_optional_role));
    require(wrong_optional_role_result.status == StaticSceneResourceStatus::invalid_request &&
                wrong_optional_role_result.diagnostic.code == "stock_material_resource_unsupported",
            "damage dust slot cannot be replaced by generic detail");

    Fixture duplicate_dust = damage_fixture();
    duplicate_dust.packets.front().resources.back().slot = "txDust";
    duplicate_dust.packets.front().resources[3].slot = "txDust";
    const auto duplicate_dust_result =
        prepare_stock_material_execution(device, request_for(duplicate_dust));
    require(duplicate_dust_result.status == StaticSceneResourceStatus::invalid_request &&
                duplicate_dust_result.diagnostic.code == "stock_material_resource_duplicate",
            "duplicate damage dust slots are rejected before pipeline creation");

    Fixture dirty_damage = damage_fixture(0.25F);
    const auto dirty_damage_result =
        prepare_stock_material_execution(device, request_for(dirty_damage));
    require(dirty_damage_result.status == StaticSceneResourceStatus::unsupported &&
                dirty_damage_result.diagnostic.code ==
                    "ks_per_pixel_damage_dirt_unsupported",
            "nonzero damage dirt remains outside the exact handoff");
    const std::size_t calls_before_base_bad_target = device.buffer_calls;
    auto base_bad_target = request_for(base_at_fixture);
    base_bad_target.targets.colors.front().samples = 1U;
    base_bad_target.targets.depth.samples = 1U;
    const auto base_bad_target_result =
        prepare_stock_material_execution(device, base_bad_target);
    require(base_bad_target_result.status != StaticSceneResourceStatus::ready,
            "base AT MultiMap must reject a non-four-sample target");
    require(device.buffer_calls == calls_before_base_bad_target,
            "base AT target mismatch must fail before backend allocation");

    Fixture fixture_value = fixture("ksPerPixelMultiMap_AT_NMDetail");
    const auto result = prepare_stock_material_execution(device, request_for(fixture_value));
    if (!result.ok())
        throw std::runtime_error("AT detail handoff failed: " + result.diagnostic.code + " " + result.diagnostic.message);
    require(result.resources->draw_count() == 1U, "handoff must retain the packet");
    require(device.buffer_calls != 0U, "successful handoff must allocate owned scene resources");

    const std::size_t calls_before_bad_target = device.buffer_calls;
    auto bad_target = request_for(fixture_value);
    bad_target.targets.colors.front().samples = 1U;
    bad_target.targets.depth.samples = 1U;
    const auto bad_target_result = prepare_stock_material_execution(device, bad_target);
    require(bad_target_result.status != StaticSceneResourceStatus::ready,
            "AT must reject a non-four-sample target");
    require(device.buffer_calls == calls_before_bad_target,
            "target sample mismatch must be rejected before backend allocation");
}

void test_skinned_cross_backend_family_authority() {
    for (const Backend backend : {Backend::Vulkan, Backend::D3D12}) {
        FakeDevice device(backend);
        Fixture family = fixture("ksSkinnedMesh", backend);
        Fixture named = fixture("ksSkinnedMesh", backend);
        for (auto& module : named.modules)
            module.bytes = shader_fixture(backend, true);
        named.module_set.key_kind =
            StockMaterialShaderKeyKind::material_name;
        named.module_set.key = "seat_material";

        const std::array<StockMaterialShaderModules, 2U> sets = {
            named.module_set, family.module_set};
        auto request = request_for(family);
        request.shader_modules = sets;
        request.limits.scene.pipeline.max_total_shader_bytes = 100U;
        const auto result = prepare_stock_material_execution(device, request);
        require(result.ok() && result.resources->backend() == backend &&
                    result.resources->draw_count() == 1U &&
                    result.resources->unique_geometry_count() == 1U &&
                    result.resources->prepared_packets().size() == 1U &&
                    result.resources->prepared_packets()[0].primitive ==
                        DrawPrimitiveKind::skinned_mesh &&
                    result.resources->prepared_packets()[0]
                            .vertex_stride_floats == 19U,
                "skinned family modules pass Vulkan and D3D12 preflight");

        FakeDevice wrong_format_device(backend);
        Fixture wrong_format = fixture(
            "ksSkinnedMesh", backend == Backend::Vulkan
                                 ? Backend::D3D12
                                 : Backend::Vulkan);
        const auto wrong = prepare_stock_material_execution(
            wrong_format_device, request_for(wrong_format));
        require(wrong.status == StaticSceneResourceStatus::invalid_request &&
                    wrong.diagnostic.code ==
                        "stock_material_shader_module_format" &&
                    wrong_format_device.buffer_calls == 0U,
                "skinned module format rejects before backend allocation");
    }
}

void test_preflight_failures() {
    FakeDevice device;
    Fixture no_module = fixture("ksPerPixel");
    no_module.module_set.modules = {};
    auto no_module_request = request_for(no_module);
    no_module_request.shader_modules = {};
    auto result = prepare_stock_material_execution(device, no_module_request);
    require(result.diagnostic.code == "stock_material_shader_module_missing",
            "missing shader modules need a precise diagnostic");

    Fixture missing = fixture("ksPerPixel");
    missing.module_set.modules = {};
    result = prepare_stock_material_execution(device, request_for(missing));
    require(result.diagnostic.code == "stock_material_shader_modules_incomplete", "malformed modules need a precise diagnostic");
    require(device.buffer_calls == 0U, "module failures must preflight before allocation");

    Fixture truncated = fixture("ksPerPixel");
    truncated.modules.front().bytes.resize(3U);
    result = prepare_stock_material_execution(device, request_for(truncated));
    require(result.diagnostic.code == "stock_material_pipeline_invalid",
            "truncated executable modules need a pipeline diagnostic");
    require(device.buffer_calls == 0U,
            "truncated executable modules must fail before backend allocation");

    Fixture bounded = fixture("ksPerPixelMultiMap_AT_NMDetail");
    auto bounded_request = request_for(bounded);
    bounded_request.limits.scene.max_preparation_bytes = 1U;
    result = prepare_stock_material_execution(device, bounded_request);
    require(result.diagnostic.code == "stock_material_preparation_limit",
            "adapter-owned copies must consume the preparation budget");
    require(device.buffer_calls == 0U,
            "adapter preparation limits must fail before backend allocation");

    Fixture too_many_sets = fixture("ksPerPixel");
    auto too_many_sets_request = request_for(too_many_sets);
    too_many_sets_request.limits.max_shader_sets = 0U;
    result = prepare_stock_material_execution(device, too_many_sets_request);
    require(result.diagnostic.code == "stock_material_shader_set_limit",
            "shader-set limits must be checked before table scanning");
    require(device.buffer_calls == 0U,
            "shader-set limits must fail before backend allocation");

    Fixture incomplete = fixture("ksPerPixelMultiMap_NMDetail");
    incomplete.packets.front().resources.pop_back();
    result = prepare_stock_material_execution(device, request_for(incomplete));
    require(result.diagnostic.code == "stock_material_resources_incomplete", "incomplete resources need a precise diagnostic");

    Fixture incomplete_base = fixture("ksPerPixelMultiMap");
    incomplete_base.packets.front().resources.pop_back();
    result = prepare_stock_material_execution(device, request_for(incomplete_base));
    require(result.diagnostic.code == "stock_material_resources_incomplete",
            "base MultiMap requires txDiffuse, txNormal, and txMaps");

    Fixture duplicate_base = fixture("ksPerPixelMultiMap");
    duplicate_base.packets.front().resources.back().slot = "txNormal";
    result = prepare_stock_material_execution(device, request_for(duplicate_base));
    require(result.diagnostic.code == "stock_material_resource_duplicate",
            "base MultiMap rejects a duplicated texture role");

    Fixture detailed_base = fixture("ksPerPixelMultiMap");
    detailed_base.model.materials.front().properties.push_back(
        {"useDetail", 1.0F, {}, {}, {}});
    result = prepare_stock_material_execution(device, request_for(detailed_base));
    require(result.diagnostic.code ==
                "ks_per_pixel_multimap_detail_unsupported",
            "base MultiMap rejects active generic detail before allocation");

    Fixture invalid_shadow_alpha = fixture("ksPerPixelMultiMap_AT");
    invalid_shadow_alpha.model.materials.front().properties.back().value = 1.01F;
    const std::size_t calls_before_invalid_shadow_alpha = device.buffer_calls;
    result = prepare_stock_material_execution(
        device, request_for(invalid_shadow_alpha));
    require(result.status == StaticSceneResourceStatus::invalid_request &&
                result.diagnostic.code == "ks_alpha_ref_range" &&
                device.buffer_calls == calls_before_invalid_shadow_alpha,
            "out-of-range stock shadow alpha fails before backend allocation");

    Fixture non_finite_shadow_alpha = fixture("ksPerPixelMultiMap_AT");
    non_finite_shadow_alpha.model.materials.front().properties.back().value =
        std::numeric_limits<float>::quiet_NaN();
    const std::size_t calls_before_non_finite_shadow_alpha = device.buffer_calls;
    result = prepare_stock_material_execution(
        device, request_for(non_finite_shadow_alpha));
    require(result.status == StaticSceneResourceStatus::invalid_request &&
                result.diagnostic.code == "non_finite_property" &&
                device.buffer_calls == calls_before_non_finite_shadow_alpha,
            "non-finite stock shadow alpha fails before backend allocation");

    Fixture unsupported = fixture("ksPerPixelMultiMapSimpleRefl");
    result = prepare_stock_material_execution(device, request_for(unsupported));
    require(result.diagnostic.code == "stock_material_family_unsupported", "unsupported families must not be relabeled");

    Fixture override_fixture = fixture("ksPerPixel");
    std::vector<MaterialBindingOverrides> overrides(1U);
    overrides.front().properties["ksDiffuse"] = MaterialPropertyOverride::scalar_value(0.4F);
    auto override_request = request_for(override_fixture);
    override_request.overrides_by_material = overrides;
    result = prepare_stock_material_execution(device, override_request);
    require(result.ok(), "bounded property overrides should be handed off");
    bool found_override = false;
    for (const auto& bytes : device.initial_buffers) {
        if (bytes.size() < sizeof(KsPerPixelMaterialConstants)) continue;
        KsPerPixelMaterialConstants constants{};
        std::memcpy(&constants, bytes.data(), sizeof(constants));
        if (std::abs(constants.lighting[1] - 0.4F) < 0.0001F) {
            found_override = true;
            break;
        }
    }
    require(found_override, "bounded property overrides must reach the owned material record");

    overrides.front().properties.clear();
    overrides.front().resources["txDiffuse"].file = "outside.dds";
    result = prepare_stock_material_execution(device, override_request);
    require(result.diagnostic.code == "stock_material_binding_incomplete" ||
                result.diagnostic.code == "stock_material_texture_override_unsupported",
            "external texture overrides need an explicit boundary");
}

void test_directional_shadow_receiver_module_opt_in() {
    FakeDevice device;

    Fixture non_receiver = fixture("ksPerPixel");
    auto non_receiver_request = request_for(non_receiver);
    non_receiver_request.directional_shadow_receiver = true;
    const auto rejected = prepare_stock_material_execution(
        device, non_receiver_request);
    require(rejected.status == StaticSceneResourceStatus::unsupported &&
                rejected.diagnostic.code == "stock_material_shader_module_missing" &&
                device.buffer_calls == 0U,
            "receiver-enabled requests reject non-receiver modules before allocation");

    Fixture receiver = fixture("ksPerPixel");
    receiver.module_set.directional_shadow_receiver = true;
    auto receiver_request = request_for(receiver);
    receiver_request.directional_shadow_receiver = true;
    const auto accepted = prepare_stock_material_execution(
        device, receiver_request);
    require(accepted.ok(),
            "receiver-enabled requests accept matching receiver modules");

    // A receiver and non-receiver set may coexist for the same key and
    // variant. The receiver flag is part of module-set uniqueness and
    // selection, so this must still select the matching receiver set.
    Fixture ordinary = fixture("ksPerPixel");
    Fixture matching = fixture("ksPerPixel");
    matching.module_set.directional_shadow_receiver = true;
    const std::array<StockMaterialShaderModules, 2> variants = {
        ordinary.module_set, matching.module_set};
    auto both_request = request_for(matching);
    both_request.shader_modules = variants;
    both_request.directional_shadow_receiver = true;
    const auto both = prepare_stock_material_execution(device, both_request);
    require(both.ok(),
            "receiver and non-receiver module sets have independent uniqueness keys");
}

} // namespace

int main() {
    try {
        test_success_and_a2c();
        test_skinned_cross_backend_family_authority();
        test_preflight_failures();
        test_directional_shadow_receiver_module_opt_in();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
