#include "apex/render/stock_scene_execution.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
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
    explicit FakeBuffer(BufferDescription description) : info_{{description}} {}
    Backend backend() const noexcept override { return Backend::Vulkan; }
    const BufferInfo& info() const noexcept override { return info_; }
private:
    BufferInfo info_{};
};

class FakeDevice final : public Device {
public:
    const DeviceInfo& info() const noexcept override { return info_; }
    BufferResult create_buffer(const BufferDescription& description,
                               std::span<const std::byte> initial_data) override {
        ++buffer_calls;
        initial_buffers.emplace_back(initial_data.begin(), initial_data.end());
        return {BufferStatus::ready, {}, std::make_unique<FakeBuffer>(description)};
    }
    BufferUpdateResult update_buffer(Buffer&, std::uint64_t,
                                     std::span<const std::byte>) override {
        return {BufferStatus::ready, {}};
    }
    TextureResult create_texture(const TextureDescription&, const TextureUploadPlan&) override {
        return {TextureStatus::unsupported, {"unused", "unused"}, nullptr};
    }
    TextureUpdateResult update_texture(Texture&, const TextureUploadPlan&) override {
        return {TextureStatus::unsupported, {"unused", "unused"}};
    }
    TextureClearReadbackResult clear_texture_and_readback(
        Texture&, const TextureClearReadbackRequest&) override {
        return {TextureReadbackStatus::unsupported, {"unused", "unused"}, {}};
    }
    TriangleDrawResult draw_triangle_and_readback(Texture&, const TriangleDrawRequest&) override {
        return {TriangleDrawStatus::unsupported, {"unused", "unused"}, {}};
    }
    SamplerResult create_sampler(const SamplerDescription&) override {
        return {SamplerStatus::unsupported, {"unused", "unused"}, nullptr};
    }
    ShaderModuleResult create_shader_module(const ShaderModuleDescription&) override {
        return {ShaderModuleStatus::unsupported, {"unused", "unused"}, nullptr};
    }
    void wait_idle() noexcept override {}

    std::size_t buffer_calls = 0U;
    std::vector<std::vector<std::byte>> initial_buffers;
private:
    DeviceInfo info_{Backend::Vulkan, "fake", "unit", 1U, 0U, 0U, 0U, 0U, true};
};

std::vector<std::uint8_t> shader_fixture() {
    constexpr std::array<std::uint32_t, 29> words = {
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
            bytes[index * 4U + byte] = static_cast<std::uint8_t>(words[index] >> (byte * 8U));
    return bytes;
}

struct Fixture {
    apex::formats::Kn5File model;
    apex::scene::SceneSnapshot scene;
    std::vector<PipelineShaderModule> modules;
    StockMaterialShaderModules module_set;
};

void add_mesh(Fixture& fixture_value, std::string name, std::uint32_t material,
              bool transparent, float x) {
    apex::formats::Kn5Node mesh;
    mesh.type = 2U;
    mesh.kind = "mesh";
    mesh.name = name;
    mesh.vertexStride = 11U;
    mesh.materialId = material;
    mesh.vertices.resize(33U, 0.0F);
    mesh.vertices[0] = -0.5F;
    mesh.vertices[11U] = 0.5F;
    mesh.vertices[22U] = 0.5F;
    mesh.indices = {0U, 1U, 2U};
    fixture_value.model.root.children.push_back(std::move(mesh));

    apex::scene::SceneNode node;
    node.name = std::move(name);
    node.kind = apex::scene::NodeKind::mesh;
    node.material = material;
    node.transparent = transparent;
    node.bounds_center = {x, 0.0F, 0.0F};
    (void)fixture_value.scene.add_node(std::move(node), fixture_value.scene.root);
}

Fixture fixture() {
    Fixture result;
    result.model.materials.resize(1U);
    result.model.materials.front().name = "body";
    result.model.materials.front().shader = "ksPerPixel";
    result.model.materials.front().resources.push_back({"txDiffuse", 0U, "diffuse"});
    result.model.textures.push_back({true, "diffuse", 4U, {}, std::nullopt});
    result.model.root.type = 1U;
    result.model.root.kind = "node";
    result.model.root.name = "ROOT";
    apex::scene::SceneNode root;
    root.name = "ROOT";
    (void)result.scene.add_node(std::move(root));
    (void)result.scene.add_material({"body", "ksPerPixel", apex::scene::BlendMode::opaque});
    add_mesh(result, "Opaque", 0U, false, 2.0F);
    add_mesh(result, "Transparent", 0U, true, 8.0F);
    add_mesh(result, "Hidden", 0U, false, 4.0F);
    result.scene.nodes.back().visible = false;
    add_mesh(result, "OutOfLod", 0U, false, 20.0F);
    result.scene.nodes.back().lod_in = 100.0F;
    add_mesh(result, "OpaqueLayer", 0U, false, 3.0F);
    result.scene.nodes.back().layer = 1U;
    add_mesh(result, "AtLodBoundary", 0U, false, 10.0F);
    result.scene.nodes.back().lod_in = 10.0F;
    result.scene.nodes.back().lod_out = 10.0F;
    add_mesh(result, "TransparentTie", 0U, true, 8.0F);
    const std::vector<std::uint8_t> bytes = shader_fixture();
    result.modules = {{PipelineShaderStage::vertex, PipelineShaderFormat::spirv, bytes},
                      {PipelineShaderStage::fragment, PipelineShaderFormat::spirv, bytes}};
    result.module_set = {StockMaterialShaderKeyKind::shader_family, "ksPerPixel", result.modules};
    return result;
}

Fixture damage_fixture() {
    Fixture result = fixture();
    auto& material = result.model.materials.front();
    material.shader = "ksPerPixelMultiMap_damage_dirt";
    material.properties = {
        {"damageZones", 0.0F, {}, {}, {0.0F, 0.0F, 0.0F, 0.0F}},
        {"dirt", 0.0F, {}, {}, {}},
        {"glassDamage", 0.25F, {}, {}, {}},
        {"fresnelMaxLevel", 0.0F, {}, {}, {}},
    };
    material.resources = {
        {"txDiffuse", 0U, "diffuse"},
        {"txNormal", 1U, "normal"},
        {"txMaps", 2U, "maps"},
        {"txDamage", 4U, "damage"},
        {"txDamageMask", 21U, "damage_mask"},
        {"txDust", 5U, "dust"},
    };
    result.model.textures.clear();
    for (const std::string name : {"diffuse", "normal", "maps", "damage",
                                   "damage_mask", "dust"})
        result.model.textures.push_back({true, name, 4U, {}, std::nullopt});
    result.model.root.children.front().name = "DAMAGE_GLASS_FRONT_1";
    result.model.root.children.front().active = false;
    result.scene.nodes[1U].name = "DAMAGE_GLASS_FRONT_1";
    result.scene.nodes[1U].active = false;
    result.scene.materials.front().shader = material.shader;
    result.module_set.key = material.shader;
    result.module_set.variant = StockMaterialShaderVariant::damage_dust;
    return result;
}

StockSceneExecutionRequest request_for(Fixture& fixture_value) {
    StockSceneExecutionRequest request;
    request.model = &fixture_value.model;
    request.scene = &fixture_value.scene;
    request.shader_modules = std::span<const StockMaterialShaderModules>(
        &fixture_value.module_set, 1U);
    request.targets.colors.push_back({PipelineRenderTargetFormat::rgba8_unorm, 1U});
    request.targets.has_depth = true;
    request.targets.depth = {PipelineRenderTargetFormat::depth32_float, 1U};
    return request;
}

void test_success_and_plan_evidence() {
    Fixture fixture_value = fixture();
    FakeDevice device;
    const auto result = prepare_stock_scene_execution(device, request_for(fixture_value));
    require(result.ok(), "bounded stock-scene handoff should succeed");
    require(result.render_plan.items.size() == 5U, "visible scene items must reach the plan");
    const auto item_name = [&](std::size_t index) {
        return fixture_value.scene.find_node(result.render_plan.items[index].node)->name;
    };
    require(item_name(0) == "Opaque" && item_name(1) == "AtLodBoundary" &&
                item_name(2) == "OpaqueLayer",
            "opaque ordering must retain layer order and inclusive LOD boundaries");
    require(item_name(3) == "Transparent" && item_name(4) == "TransparentTie",
            "transparent ordering must follow opaque ordering and retain equal-distance stability");
    require(!result.render_plan.unsupported_effects.empty(),
            "staged frame effects must remain visible as evidence");
    require(!result.packet_unsupported_effects.empty(),
            "staged packet shader/texture effects must remain visible as evidence");
    require(std::any_of(result.render_plan.unsupported_effects.begin(),
                        result.render_plan.unsupported_effects.end(),
                        [](const UnsupportedEffect& effect) {
                            return effect.code == "stock_scene_snapshot_staged";
                        }),
            "pre-resolved snapshot limitations must remain explicit");
    require(device.buffer_calls != 0U, "successful scene preparation must allocate resources");
}

void test_resolved_subtree_filter_and_isolation_reach_facade() {
    Fixture fixture_value = fixture();
    FakeDevice device;
    auto request = request_for(fixture_value);
    const std::array<apex::scene::NodeId, 1U> excluded = {
        fixture_value.scene.nodes[1U].id};
    request.render.excluded_subtree_roots = excluded;
    auto result = prepare_stock_scene_execution(device, request);
    require(result.ok(), "resolved subtree filter should reach stock-scene execution");
    require(result.render_plan.items.size() == 4U &&
                std::none_of(result.render_plan.items.begin(), result.render_plan.items.end(),
                             [&](const RenderItem& item) {
                                 return item.node == excluded.front();
                             }),
            "resolved subtree root must be absent from facade packets");

    request = request_for(fixture_value);
    request.render.show_hidden = true;
    result = prepare_stock_scene_execution(device, request);
    require(result.ok() && result.render_plan.items.size() == 6U,
            "show-hidden should reach the facade without bypassing mesh LOD");

    request = request_for(fixture_value);
    const std::array<apex::scene::NodeActivityOverride, 1U> inactive = {
        apex::scene::NodeActivityOverride{fixture_value.scene.nodes[1U].id, false}};
    request.render.activity_overrides = inactive;
    result = prepare_stock_scene_execution(device, request);
    require(result.ok() && result.render_plan.items.size() == 4U,
            "preview activity overrides should reach the stock-scene facade");

    request = request_for(fixture_value);
    const std::array<apex::scene::NodeId, 1U> suppressed = {fixture_value.scene.nodes[1U].id};
    request.render.show_hidden = true;
    request.render.suppressed_subtree_roots = suppressed;
    result = prepare_stock_scene_execution(device, request);
    require(result.ok() && result.render_plan.items.size() == 5U,
            "driver-style suppression should remain active during show-hidden");

    request = request_for(fixture_value);
    const std::array<apex::scene::NodeId, 1U> isolated_suppressed = {
        fixture_value.scene.nodes[3U].id};
    request.render.suppressed_subtree_roots = isolated_suppressed;
    request.render.isolated = true;
    request.render.isolated_node = fixture_value.scene.nodes[3U].id;
    result = prepare_stock_scene_execution(device, request);
    require(result.ok() && result.render_plan.items.size() == 1U &&
                result.render_plan.items[0].node == fixture_value.scene.nodes[3U].id,
            "facade isolation must bypass authored visibility and subtree filters");
}

void test_csp_node_state_reaches_per_packet_pipelines() {
    Fixture fixture_value = fixture();
    FakeDevice device;
    auto request = request_for(fixture_value);
    const std::array<NodeRenderStateOverride, 3U> overrides = {{
        {fixture_value.scene.nodes[1U].id, true, 5.5, std::nullopt,
         std::nullopt, false},
        {fixture_value.scene.nodes[2U].id, false, 0.25, std::nullopt,
         std::nullopt, std::nullopt},
        {fixture_value.scene.nodes[4U].id, std::nullopt, std::nullopt, 0.0,
         0.0, std::nullopt},
    }};
    request.render.node_state_overrides = overrides;
    const auto result = prepare_stock_scene_execution(device, request);
    require(result.ok(), "CSP node state should reach executable scene preparation");
    require(result.render_plan.items.size() == 6U,
            "CSP LOD state should change facade item inclusion");
    require(result.render_plan.transparent_items.size() == 2U,
            "explicit CSP transparency should replace authored classification");
    require(result.render_plan.shadow_casters.size() == 5U,
            "CSP cast-shadow state should reach facade shadow evidence");
    require(result.resources->unique_pipeline_count() == 2U,
            "shared materials should retain distinct per-node transparency pipelines");
}

void test_damage_preview_reaches_scene_and_material_handoff() {
    Fixture broken_fixture = damage_fixture();
    FakeDevice broken_device;
    auto broken_request = request_for(broken_fixture);
    broken_request.evaluate_damage_preview = true;
    broken_request.damage_broken_visible = true;
    const auto broken =
        prepare_stock_scene_execution(broken_device, broken_request);
    require(broken.ok() && broken.damage_preview.has_value() &&
                broken.damage_preview->executable_zero_dirt_materials ==
                    std::vector<apex::scene::MaterialId>{0U},
            "facade must retain the executable dirt-zero damage audit");
    require(broken.render_plan.items.size() == 5U,
            "broken F4 state must activate the selected damage root");
    bool found_broken_zones = false;
    for (const auto& bytes : broken_device.initial_buffers) {
        if (bytes.size() < sizeof(KsPerPixelMaterialConstants)) continue;
        KsPerPixelMaterialConstants constants{};
        std::memcpy(&constants, bytes.data(), sizeof(constants));
        found_broken_zones = found_broken_zones ||
                             constants.damage_zones ==
                                 std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F};
    }
    require(found_broken_zones,
            "broken F4 damage zones must reach the owned material record");

    Fixture intact_fixture = damage_fixture();
    FakeDevice intact_device;
    auto intact_request = request_for(intact_fixture);
    intact_request.evaluate_damage_preview = true;
    intact_request.damage_broken_visible = false;
    const auto intact =
        prepare_stock_scene_execution(intact_device, intact_request);
    require(intact.ok() && intact.render_plan.items.size() == 4U,
            "intact F4 state must keep the selected damage root inactive");
    bool found_intact_zones = false;
    for (const auto& bytes : intact_device.initial_buffers) {
        if (bytes.size() < sizeof(KsPerPixelMaterialConstants)) continue;
        KsPerPixelMaterialConstants constants{};
        std::memcpy(&constants, bytes.data(), sizeof(constants));
        found_intact_zones = found_intact_zones ||
                             constants.damage_zones ==
                                 std::array<float, 4>{0.0F, 0.0F, 0.0F, 0.0F};
    }
    require(found_intact_zones,
            "intact F4 damage zones must reach the owned material record");

    Fixture conflict_fixture = damage_fixture();
    FakeDevice conflict_device;
    auto conflict_request = request_for(conflict_fixture);
    conflict_request.evaluate_damage_preview = true;
    conflict_request.damage_broken_visible = true;
    const std::array<apex::scene::NodeActivityOverride, 1U> caller_activity = {{
        {conflict_fixture.scene.nodes[1U].id, false},
    }};
    conflict_request.render.activity_overrides = caller_activity;
    const auto conflict =
        prepare_stock_scene_execution(conflict_device, conflict_request);
    require(conflict.status == StaticSceneResourceStatus::invalid_request &&
                conflict.diagnostic.code == "stock_scene_damage_activity_conflict" &&
                conflict_device.buffer_calls == 0U,
            "caller and F4 activity conflicts must fail before backend allocation");

    Fixture limited_fixture = damage_fixture();
    FakeDevice limited_device;
    auto limited_request = request_for(limited_fixture);
    limited_request.evaluate_damage_preview = true;
    limited_request.damage_broken_visible = true;
    limited_request.limits.damage.max_output_bytes = 1U;
    const auto limited =
        prepare_stock_scene_execution(limited_device, limited_request);
    require(limited.status == StaticSceneResourceStatus::invalid_request &&
                limited.damage_preview.has_value() &&
                limited.damage_preview->limit_exceeded &&
                limited_device.buffer_calls == 0U,
            "bounded damage resolution must fail before backend allocation");
}

void test_preflight_and_missing_modules() {
    Fixture fixture_value = fixture();
    FakeDevice device;
    auto request = request_for(fixture_value);
    request.shader_modules = {};
    const auto missing = prepare_stock_scene_execution(device, request);
    require(missing.status == StaticSceneResourceStatus::unsupported,
            "missing modules must be reported as unsupported");
    require(device.buffer_calls == 0U, "missing modules must fail before backend allocation");
    require(!missing.render_plan.items.empty(), "bounded plan evidence must be retained on failure");

    request = request_for(fixture_value);
    request.limits.max_scene_nodes = 1U;
    const auto count_limited = prepare_stock_scene_execution(device, request);
    require(count_limited.diagnostic.code == "stock_scene_node_limit",
            "scene count limits need a precise diagnostic");
    require(device.buffer_calls == 0U, "scene count failures must precede backend allocation");

    request = request_for(fixture_value);
    request.wireframe = true;
    const auto wireframe_mismatch = prepare_stock_scene_execution(device, request);
    require(wireframe_mismatch.diagnostic.code == "stock_scene_wireframe_mismatch",
            "wireframe state must have one authoritative value");
    require(device.buffer_calls == 0U, "wireframe mismatch must precede backend allocation");

    request = request_for(fixture_value);
    request.limits.max_plan_bytes = 1U;
    const auto byte_limited = prepare_stock_scene_execution(device, request);
    require(byte_limited.diagnostic.code == "stock_scene_plan_preflight_limit",
            "pre-plan byte limits need a precise diagnostic");
    require(byte_limited.render_plan.items.empty() &&
                byte_limited.render_plan.unsupported_effects.empty(),
            "pre-plan budget failures must occur before plan allocation");
    require(device.buffer_calls == 0U,
            "pre-plan byte limits must precede backend allocation");

    apex::scene::SceneSnapshot inconsistent_scene = fixture_value.scene;
    inconsistent_scene.nodes.front().children.erase(
        inconsistent_scene.nodes.front().children.begin());
    request = request_for(fixture_value);
    request.scene = &inconsistent_scene;
    const auto inconsistent = prepare_stock_scene_execution(device, request);
    require(inconsistent.diagnostic.code == "stock_scene_topology_invalid",
            "parent IDs without matching child edges must be rejected");
    require(device.buffer_calls == 0U,
            "inconsistent topology must fail before backend allocation");

    request = request_for(fixture_value);
    const std::array<apex::scene::NodeActivityOverride, 1U> invalid_override = {
        apex::scene::NodeActivityOverride{apex::scene::invalid_node_id, false}};
    request.render.activity_overrides = invalid_override;
    const auto invalid_activity = prepare_stock_scene_execution(device, request);
    require(invalid_activity.diagnostic.code == "stock_scene_activity_override_invalid",
            "invalid activity overrides need a precise diagnostic");

    request = request_for(fixture_value);
    const std::array<apex::scene::NodeActivityOverride, 2U> duplicate_overrides = {
        apex::scene::NodeActivityOverride{fixture_value.scene.nodes[1U].id, false},
        apex::scene::NodeActivityOverride{fixture_value.scene.nodes[1U].id, true}};
    request.render.activity_overrides = duplicate_overrides;
    const auto duplicate_activity = prepare_stock_scene_execution(device, request);
    require(duplicate_activity.diagnostic.code == "stock_scene_activity_override_duplicate",
            "duplicate activity overrides need a precise diagnostic");

    request = request_for(fixture_value);
    const std::array<apex::scene::NodeId, 1U> invalid_exclusion = {apex::scene::invalid_node_id};
    request.render.excluded_subtree_roots = invalid_exclusion;
    const auto invalid_excluded = prepare_stock_scene_execution(device, request);
    require(invalid_excluded.diagnostic.code == "stock_scene_exclusion_invalid",
            "invalid subtree exclusions need a precise diagnostic");

    request = request_for(fixture_value);
    const std::array<apex::scene::NodeId, 2U> duplicate_suppression = {
        fixture_value.scene.nodes[1U].id, fixture_value.scene.nodes[1U].id};
    request.render.suppressed_subtree_roots = duplicate_suppression;
    const auto duplicate_suppressed = prepare_stock_scene_execution(device, request);
    require(duplicate_suppressed.diagnostic.code == "stock_scene_suppression_duplicate",
            "duplicate subtree suppressions need a precise diagnostic");

    request = request_for(fixture_value);
    std::vector<apex::scene::NodeId> too_many_options(fixture_value.scene.nodes.size() + 1U,
                                                      fixture_value.scene.root);
    request.render.excluded_subtree_roots = too_many_options;
    const auto option_limited = prepare_stock_scene_execution(device, request);
    require(option_limited.diagnostic.code == "stock_scene_render_option_limit",
            "render-option counts must be bounded by scene size");

    request = request_for(fixture_value);
    const std::array<NodeRenderStateOverride, 2U> duplicate_node_state = {{
        {fixture_value.scene.nodes[1U].id, true, std::nullopt, std::nullopt,
         std::nullopt, std::nullopt},
        {fixture_value.scene.nodes[1U].id, false, std::nullopt, std::nullopt,
         std::nullopt, std::nullopt},
    }};
    request.render.node_state_overrides = duplicate_node_state;
    const auto duplicate_state = prepare_stock_scene_execution(device, request);
    require(duplicate_state.diagnostic.code ==
                "stock_scene_node_state_override_duplicate",
            "duplicate node-state overrides need a precise diagnostic");

    request = request_for(fixture_value);
    const std::array<NodeRenderStateOverride, 1U> invalid_node_state = {{
        {apex::scene::invalid_node_id, true, std::nullopt, std::nullopt,
         std::nullopt, std::nullopt},
    }};
    request.render.node_state_overrides = invalid_node_state;
    const auto invalid_state = prepare_stock_scene_execution(device, request);
    require(invalid_state.diagnostic.code ==
                "stock_scene_node_state_override_invalid",
            "unknown node-state override IDs need a precise diagnostic");

    request = request_for(fixture_value);
    const std::array<NodeRenderStateOverride, 1U> non_finite_node_state = {{
        {fixture_value.scene.nodes[1U].id, std::nullopt,
         std::numeric_limits<double>::infinity(), std::nullopt, std::nullopt,
         std::nullopt},
    }};
    request.render.node_state_overrides = non_finite_node_state;
    const auto non_finite_state = prepare_stock_scene_execution(device, request);
    require(non_finite_state.diagnostic.code ==
                "stock_scene_node_state_override_non_finite",
            "non-finite node-state overrides need a precise diagnostic");

    request = request_for(fixture_value);
    request.render.isolated = true;
    request.render.isolated_node = apex::scene::invalid_node_id;
    const auto invalid_isolation = prepare_stock_scene_execution(device, request);
    require(invalid_isolation.diagnostic.code == "stock_scene_isolation_invalid",
            "invalid isolation targets need a precise diagnostic");
    require(device.buffer_calls == 0U,
            "malformed render options must fail before backend allocation");

    apex::scene::SceneSnapshot deep_scene;
    apex::scene::SceneNode deep_root;
    deep_root.name = "ROOT";
    apex::scene::NodeId parent = deep_scene.add_node(std::move(deep_root));
    for (std::size_t index = 0U; index < 20'000U; ++index) {
        apex::scene::SceneNode node;
        node.name = "deep";
        node.kind = apex::scene::NodeKind::node;
        parent = deep_scene.add_node(std::move(node), parent);
    }
    request = request_for(fixture_value);
    request.scene = &deep_scene;
    request.limits.max_plan_bytes = 4ULL * 1024ULL * 1024ULL;
    const auto deep_limited = prepare_stock_scene_execution(device, request);
    require(deep_limited.diagnostic.code == "stock_scene_plan_preflight_limit",
            "deep topology must be rejected by the bounded pre-plan estimate");
    require(deep_limited.render_plan.items.empty() &&
                deep_limited.render_plan.unsupported_effects.empty() &&
                device.buffer_calls == 0U,
            "deep pre-plan rejection must allocate neither plan evidence nor backend resources");
}

}  // namespace

int main() {
    try {
        test_success_and_plan_evidence();
        test_resolved_subtree_filter_and_isolation_reach_facade();
        test_csp_node_state_reaches_per_packet_pipelines();
        test_damage_preview_reaches_scene_and_material_handoff();
        test_preflight_and_missing_modules();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
