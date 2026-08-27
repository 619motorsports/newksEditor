#include "apex/render/damage_preview.hpp"
#include "apex/render/render_plan.hpp"
#include "apex/scene/kn5_scene.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

apex::formats::Kn5Matrix4 identity() {
    return {
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F,
    };
}

apex::formats::Kn5Node group(std::string name, bool active = false) {
    apex::formats::Kn5Node value;
    value.type = 1U;
    value.kind = "node";
    value.name = std::move(name);
    value.active = active;
    value.transform = identity();
    return value;
}

apex::formats::Kn5Node mesh(std::string name, std::uint32_t material) {
    apex::formats::Kn5Node value;
    value.type = 2U;
    value.kind = "mesh";
    value.name = std::move(name);
    value.active = true;
    value.visible = true;
    value.renderable = true;
    value.castShadows = true;
    value.transform = identity();
    value.vertexStride = 11U;
    value.vertices.resize(33U, 0.0F);
    value.indices = {0U, 1U, 2U};
    value.materialId = material;
    return value;
}

apex::formats::Kn5MaterialProperty scalar(std::string name, float value) {
    apex::formats::Kn5MaterialProperty property;
    property.name = std::move(name);
    property.value = value;
    property.value2 = {value, value};
    property.value3 = {value, value, value};
    property.value4 = {value, value, value, value};
    return property;
}

apex::formats::Kn5Material damage_material(std::string name, bool glass) {
    apex::formats::Kn5Material material;
    material.name = std::move(name);
    material.shader = "ksPerPixelMultiMap_damage_dirt";
    material.properties.push_back(scalar("damageZones", 0.0F));
    material.properties.push_back(scalar("dirt", 0.0F));
    material.properties.push_back(scalar("fresnelMaxLevel", 0.0F));
    if (glass) material.properties.push_back(scalar("glassDamage", 0.25F));
    material.resources.push_back({"txDiffuse", 0U, "diffuse.dds"});
    material.resources.push_back({"txNormal", 1U, "normal.dds"});
    material.resources.push_back({"txMaps", 2U, "maps.dds"});
    material.resources.push_back({"txDamage", 4U, "damage.dds"});
    material.resources.push_back({"txDamageMask", 21U, "damage_mask.dds"});
    return material;
}

struct Fixture {
    apex::formats::Kn5File model;
    apex::scene::SceneSnapshot scene;
};

Fixture fixture() {
    Fixture value;
    value.model.materials.push_back(damage_material("Damage", true));
    value.model.materials.push_back(damage_material("SharedGlass", true));
    value.model.materials.push_back(damage_material("OtherDamage", false));
    value.model.root = group("ROOT", true);

    auto front1 = group("DAMAGE_GLASS_FRONT_1");
    auto inactive = group("INACTIVE_PARENT");
    inactive.children.push_back(mesh("FrontMesh", 1U));
    front1.children.push_back(std::move(inactive));
    value.model.root.children.push_back(std::move(front1));

    auto duplicate = group("DAMAGE_GLASS_FRONT_1");
    duplicate.children.push_back(mesh("DuplicateMesh", 0U));
    value.model.root.children.push_back(std::move(duplicate));

    auto front2 = group("DAMAGE_GLASS_FRONT_2");
    front2.children.push_back(mesh("FrontMesh2", 0U));
    value.model.root.children.push_back(std::move(front2));

    auto front4 = group("DAMAGE_GLASS_FRONT_4");
    front4.children.push_back(mesh("IgnoredAfterGap", 0U));
    value.model.root.children.push_back(std::move(front4));

    for (const std::string_view name : {"DAMAGE_GLASS_REAR_1", "DAMAGE_GLASS_LEFT_1",
                                        "DAMAGE_GLASS_RIGHT_1", "DAMAGE_GLASS_CENTER_1"}) {
        auto node = group(std::string(name));
        node.children.push_back(mesh(std::string(name) + "_MESH", 0U));
        value.model.root.children.push_back(std::move(node));
    }
    auto lower = group("damage_glass_front_3");
    lower.children.push_back(mesh("LowerCaseIgnored", 0U));
    value.model.root.children.push_back(std::move(lower));
    auto leading_zero = group("DAMAGE_GLASS_FRONT_03");
    leading_zero.children.push_back(mesh("LeadingZeroIgnored", 0U));
    value.model.root.children.push_back(std::move(leading_zero));
    value.model.root.children.push_back(mesh("UnrelatedSharedDraw", 1U));

    value.scene = apex::scene::convertKn5ToScene(value.model);
    return value;
}

const apex::render::MaterialPropertyOverride* property(
    const apex::render::MaterialBindingOverrides& overrides,
    std::string_view name) {
    const auto found = overrides.properties.find(std::string(name));
    return found == overrides.properties.end() ? nullptr : &found->second;
}

apex::scene::NodeId node_id(const apex::scene::SceneSnapshot& scene,
                            std::string_view name) {
    const auto found = std::find_if(scene.nodes.begin(), scene.nodes.end(),
                                    [name](const auto& node) { return node.name == name; });
    return found == scene.nodes.end() ? apex::scene::invalid_node_id : found->id;
}

void resolves_exact_numbered_groups() {
    const Fixture value = fixture();
    const auto result = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, std::nullopt, {}});
    require(result.ok() && result.available, "authored damage audit should be available");
    require(result.groups.size() == 5U && result.selected_roots.size() == 6U,
            "five exact prefix groups should select through the first gap");
    require(result.groups[0U].selected_roots.size() == 2U &&
                result.groups[0U].first_missing == 3U &&
                result.groups[0U].ignored_later_nodes == 1U,
            "front numbering should use first duplicates and stop at suffix three");
    require(result.groups[1U].prefix == "DAMAGE_GLASS_REAR_" &&
                result.groups[4U].prefix == "DAMAGE_GLASS_CENTER_",
            "native prefix order should be preserved");
    require(result.diagnostics.size() == 2U &&
                result.diagnostics[0U].code == "DAMAGE_PREVIEW_DUPLICATE_ROOT" &&
                result.diagnostics[1U].code == "DAMAGE_PREVIEW_NUMBERING_GAP",
            "duplicates and later roots should produce bounded diagnostics");
    require(result.activity_overrides.empty(),
            "authored audit should not replace node activity");
    require(result.damage_zone_materials.size() == 3U &&
                result.executable_zero_dirt_materials.size() == 3U,
            "bounded dirt-zero material classification should require supported properties and textures");
}

void resolves_transient_writes_without_mutation() {
    const Fixture value = fixture();
    std::vector<apex::render::MaterialBindingOverrides> base(value.model.materials.size());
    base[0U].properties.emplace(
        " DamageZones ",
        apex::render::MaterialPropertyOverride::vector4_value({0.2F, 0.3F, 0.4F, 0.5F}));
    base[0U].properties.emplace(
        "unrelated", apex::render::MaterialPropertyOverride::scalar_value(7.0F));

    const auto broken = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, true, base});
    require(broken.ok() && broken.activity_overrides.size() == broken.selected_roots.size() &&
                std::all_of(broken.activity_overrides.begin(), broken.activity_overrides.end(),
                            [](const auto& item) { return item.active; }),
            "broken F4 state should activate only selected roots");
    require(broken.affected_glass_materials.size() == 2U &&
                broken.affected_glass_materials[0U] == 0U &&
                broken.affected_glass_materials[1U] == 1U,
            "selected descendants should deduplicate shared material identities");
    const auto* zones = property(broken.material_overrides[0U], "damageZones");
    const auto* glass = property(broken.material_overrides[1U], "glassDamage");
    require(zones != nullptr && zones->kind == apex::render::MaterialPropertyOverride::Kind::vector4 &&
                zones->vector4 == std::array<float, 4U>{1.0F, 1.0F, 1.0F, 1.0F},
            "broken F4 state should replace canonical damageZones after CSP");
    require(glass != nullptr && glass->scalar == 1.0F &&
                property(broken.material_overrides[0U], "unrelated") != nullptr,
            "one-way glassDamage writes should preserve unrelated overrides");
    require(broken.material_overrides[0U].properties.size() == 3U &&
                broken.material_overrides[0U].properties.find(" DamageZones ") ==
                    broken.material_overrides[0U].properties.end(),
            "canonical runtime writes should replace case variants instead of duplicating them");

    const auto intact = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, false, base});
    zones = property(intact.material_overrides[0U], "damageZones");
    glass = property(intact.material_overrides[1U], "glassDamage");
    require(std::all_of(intact.activity_overrides.begin(), intact.activity_overrides.end(),
                        [](const auto& item) { return !item.active; }) &&
                zones != nullptr && zones->vector4 == std::array<float, 4U>{} &&
                glass != nullptr && glass->scalar == 1.0F,
            "intact F4 state should hide roots and zero zones while retaining the native one-way glass write");
    require(!value.model.root.children[0U].active &&
                value.model.materials[0U].properties[0U].value4 == std::array<float, 4U>{},
            "damage preview should not mutate parsed node or material state");

    apex::render::RenderPlanOptions plan_options;
    plan_options.activity_overrides = broken.activity_overrides;
    const auto plan = apex::render::build_render_plan(value.scene, plan_options);
    const auto visible_mesh = node_id(value.scene, "FrontMesh2");
    const auto blocked_mesh = node_id(value.scene, "FrontMesh");
    require(std::any_of(plan.items.begin(), plan.items.end(), [visible_mesh](const auto& item) {
                return item.node == visible_mesh;
            }),
            "resolved activity overrides should execute through the render planner");
    require(std::none_of(plan.items.begin(), plan.items.end(), [blocked_mesh](const auto& item) {
                return item.node == blocked_mesh;
            }),
            "an inactive non-selected ancestor should remain inactive");
}

void rejects_unsupported_damage_stage_inputs() {
    Fixture value = fixture();
    value.model.materials[2U].shader = "ksPerPixel";
    value.scene.materials[2U].shader = "ksPerPixel";
    value.model.materials[2U].resources.clear();
    std::vector<apex::render::MaterialBindingOverrides> complete(
        value.model.materials.size());
    complete[2U].shader = "ksPerPixelMultiMap_damage_dirt";
    complete[2U].properties.emplace(
        "dirt", apex::render::MaterialPropertyOverride::scalar_value(-0.0F));
    complete[2U].resources = {
        {"txDiffuse", {0U, "override_diffuse.dds", {}, {}}},
        {"txNormal", {1U, "override_normal.dds", {}, {}}},
        {"txMaps", {2U, "override_maps.dds", {}, {}}},
        {"txDamage", {4U, "override_damage.dds", {}, {}}},
        {"txDamageMask", {21U, "override_mask.dds", {}, {}}},
    };

    const auto executable = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, true, complete});
    require(executable.ok() &&
                std::find(executable.executable_zero_dirt_materials.begin(),
                          executable.executable_zero_dirt_materials.end(), 2U) !=
                    executable.executable_zero_dirt_materials.end(),
            "complete effective CSP damage resources should classify the bounded stage");

    for (const std::string_view missing_slot : {"txDiffuse", "txNormal", "txMaps"}) {
        auto missing = complete;
        auto& resources = missing[2U].resources;
        const auto resource = resources.find(std::string(missing_slot));
        require(resource != resources.end(), "test fixture must contain each required base resource");
        resources.erase(resource);
        const auto rejected = apex::render::resolve_damage_preview(
            {&value.model, &value.scene, true, missing});
        require(rejected.ok() &&
                    std::find(rejected.executable_zero_dirt_materials.begin(),
                              rejected.executable_zero_dirt_materials.end(), 2U) ==
                        rejected.executable_zero_dirt_materials.end() &&
                    std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(),
                                [](const auto& item) {
                                    return item.code == "DAMAGE_PREVIEW_STAGE_UNSUPPORTED" &&
                                           item.material == 2U;
                                }),
                "each missing base damage resource must reject bounded execution");
    }

    auto with_dust = complete;
    with_dust[2U].resources.emplace(
        "txDust", apex::render::MaterialTextureOverride{5U, "override_dust.dds", {}, {}});
    const auto dust = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, true, with_dust});
    require(dust.ok() &&
                std::find(dust.executable_zero_dirt_materials.begin(),
                          dust.executable_zero_dirt_materials.end(), 2U) !=
                    dust.executable_zero_dirt_materials.end(),
            "optional txDust must not be required for the bounded dirt-zero stage");

    for (const std::pair<std::string, float>& unsupported : {
             std::pair<std::string, float>{"useDetail", 1.0F},
             std::pair<std::string, float>{"sunSpecular", 12.0F}}) {
        auto properties = complete;
        properties[2U].properties.emplace(
            unsupported.first,
            apex::render::MaterialPropertyOverride::scalar_value(unsupported.second));
        const auto rejected = apex::render::resolve_damage_preview(
            {&value.model, &value.scene, true, properties});
        require(rejected.ok() &&
                    std::find(rejected.executable_zero_dirt_materials.begin(),
                              rejected.executable_zero_dirt_materials.end(), 2U) ==
                        rejected.executable_zero_dirt_materials.end() &&
                    std::any_of(rejected.diagnostics.begin(),
                                rejected.diagnostics.end(), [](const auto& item) {
                                    return item.code ==
                                               "DAMAGE_PREVIEW_STAGE_UNSUPPORTED" &&
                                           item.material == 2U;
                                }),
                "unsupported stock branches must not be classified as executable");
    }
}

void rejects_malformed_and_bounded_inputs() {
    Fixture value = fixture();
    value.scene.nodes[1U].name = "MISMATCH";
    auto result = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, true, {}});
    require(!result.ok() && result.selected_roots.empty() &&
                result.diagnostics.front().code == "SCENE_MODEL_IDENTITY",
            "model/scene identity mismatch should fail closed");

    value = fixture();
    value.scene.nodes[1U].children.push_back(0U);
    result = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, true, {}});
    require(!result.ok() && result.selected_roots.empty() &&
                result.diagnostics.front().code == "SCENE_MODEL_HIERARCHY",
            "malformed scene cycles should fail before preview traversal");

    value = fixture();
    value.model.root.children[0U].children[0U].children[0U].materialId =
        std::numeric_limits<std::uint32_t>::max();
    result = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, true, {}});
    require(!result.ok() && result.material_overrides.empty() &&
                result.diagnostics.front().code == "DAMAGE_PREVIEW_MATERIAL",
            "unknown untrusted material references should fail closed");

    value = fixture();
    apex::render::DamagePreviewLimits limits;
    limits.max_descendant_meshes = 1U;
    result = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, true, {}}, limits);
    require(!result.ok() && result.limit_exceeded && result.activity_overrides.empty() &&
                result.diagnostics.front().code == "DAMAGE_PREVIEW_DESCENDANT_LIMIT",
            "descendant limits should clear partial output");

    limits = {};
    limits.max_output_bytes = 1U;
    result = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, true, {}}, limits);
    require(!result.ok() && result.limit_exceeded && result.material_overrides.empty() &&
                result.diagnostics.front().code == "DAMAGE_PREVIEW_OUTPUT_LIMIT",
            "aggregate output bytes should be bounded before output escapes");

    limits = {};
    limits.max_selected_roots = 1U;
    result = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, true, {}}, limits);
    require(!result.ok() && result.limit_exceeded &&
                result.diagnostics.front().code == "DAMAGE_PREVIEW_ROOT_LIMIT",
            "selected-root counts should be bounded");

    limits = {};
    limits.max_diagnostics = 1U;
    result = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, true, {}}, limits);
    require(!result.ok() && result.limit_exceeded &&
                result.diagnostics.front().code == "DAMAGE_PREVIEW_DIAGNOSTIC_LIMIT",
            "diagnostic counts should fail closed");

    limits = {};
    limits.max_material_entries = 1U;
    result = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, true, {}}, limits);
    require(!result.ok() && result.limit_exceeded &&
                result.diagnostics.front().code == "DAMAGE_PREVIEW_MATERIAL_ENTRY_LIMIT",
            "aggregate material entries should be bounded");

    std::vector<apex::render::MaterialBindingOverrides> short_base(1U);
    limits = {};
    result = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, true, short_base}, limits);
    require(!result.ok() &&
                result.diagnostics.front().code == "DAMAGE_PREVIEW_OVERRIDE_COUNT",
            "partial material-indexed override tables should be rejected");

    value.model.materials[0U].properties[0U].value =
        std::numeric_limits<float>::quiet_NaN();
    limits = {};
    result = apex::render::resolve_damage_preview(
        {&value.model, &value.scene, true, {}}, limits);
    require(!result.ok() && result.diagnostics.front().code ==
                                "DAMAGE_PREVIEW_MATERIAL_INPUT",
            "non-finite direct material inputs should be rejected");
}

}  // namespace

int main() {
    try {
        resolves_exact_numbered_groups();
        resolves_transient_writes_without_mutation();
        rejects_unsupported_damage_stage_inputs();
        rejects_malformed_and_bounded_inputs();
        std::cout << "Damage-preview tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Damage-preview tests failed: " << error.what() << '\n';
        return 1;
    }
}
