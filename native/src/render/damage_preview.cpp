#include "apex/render/damage_preview.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <map>
#include <new>
#include <string_view>
#include <utility>

namespace apex::render {
namespace {

constexpr std::array<std::string_view, 5U> kPrefixes = {
    "DAMAGE_GLASS_FRONT_",
    "DAMAGE_GLASS_REAR_",
    "DAMAGE_GLASS_LEFT_",
    "DAMAGE_GLASS_RIGHT_",
    "DAMAGE_GLASS_CENTER_",
};

[[nodiscard]] bool charge(std::uint64_t amount, std::uint64_t& used,
                          std::uint64_t limit) noexcept {
    if (amount > limit - std::min(used, limit)) return false;
    used += amount;
    return true;
}

[[nodiscard]] std::string canonical(std::string_view value) {
    std::size_t begin = 0U;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t' ||
                                    value[begin] == '\r' || value[begin] == '\n'))
        ++begin;
    std::size_t end = value.size();
    while (end > begin && (value[end - 1U] == ' ' || value[end - 1U] == '\t' ||
                           value[end - 1U] == '\r' || value[end - 1U] == '\n'))
        --end;
    std::string result(value.substr(begin, end - begin));
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return result;
}

void clear_output(DamagePreviewResult& result) {
    result.groups.clear();
    result.selected_roots.clear();
    result.affected_glass_materials.clear();
    result.damage_zone_materials.clear();
    result.executable_zero_dirt_materials.clear();
    result.activity_overrides.clear();
    result.material_overrides.clear();
    result.diagnostics.clear();
    result.available = false;
}

void fail(DamagePreviewResult& result, std::string code, std::string message,
          bool limit_exceeded = false) {
    clear_output(result);
    result.supported = false;
    result.limit_exceeded = limit_exceeded;
    result.diagnostics.push_back({DamagePreviewDiagnosticSeverity::error,
                                  std::move(code), std::move(message)});
}

[[nodiscard]] bool add_diagnostic(DamagePreviewResult& result,
                                  const DamagePreviewLimits& limits,
                                  std::uint64_t& output_bytes,
                                  std::string code, std::string message,
                                  apex::scene::NodeId node = apex::scene::invalid_node_id,
                                  apex::scene::MaterialId material = apex::scene::invalid_material_id) {
    if (result.diagnostics.size() >= limits.max_diagnostics ||
        !charge(sizeof(DamagePreviewDiagnostic) + code.size() + message.size(),
                output_bytes, limits.max_output_bytes)) {
        fail(result, "DAMAGE_PREVIEW_DIAGNOSTIC_LIMIT",
             "Damage-preview diagnostics exceed their configured limit", true);
        return false;
    }
    result.diagnostics.push_back({DamagePreviewDiagnosticSeverity::warning,
                                  std::move(code), std::move(message), node, material});
    return true;
}

[[nodiscard]] std::optional<std::uint64_t> suffix_index(
    std::string_view name, std::string_view prefix) noexcept {
    if (!name.starts_with(prefix)) return std::nullopt;
    const std::string_view suffix = name.substr(prefix.size());
    if (suffix.empty() || suffix.front() < '1' || suffix.front() > '9')
        return std::nullopt;
    if (!std::all_of(suffix.begin(), suffix.end(),
                     [](char value) { return value >= '0' && value <= '9'; }))
        return std::nullopt;
    std::uint64_t index = 0U;
    const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
    if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size())
        return std::nullopt;
    return index;
}

[[nodiscard]] bool has_exact_property(const apex::formats::Kn5Material& material,
                                      std::string_view name) noexcept {
    return std::any_of(material.properties.begin(), material.properties.end(),
                       [name](const auto& property) { return property.name == name; });
}

[[nodiscard]] bool executable_texture(const MaterialBinding& binding,
                                      std::string_view slot) noexcept {
    const MaterialTextureBinding* texture = find_material_texture(binding, slot);
    return texture != nullptr &&
           (texture->kind == MaterialTextureKind::embedded ||
            texture->kind == MaterialTextureKind::solid_color);
}

void replace_property(MaterialBindingOverrides& overrides, std::string_view name,
                      MaterialPropertyOverride value) {
    const std::string key = canonical(name);
    for (auto item = overrides.properties.begin(); item != overrides.properties.end();) {
        if (canonical(item->first) == key)
            item = overrides.properties.erase(item);
        else
            ++item;
    }
    overrides.properties.emplace(std::string(name), std::move(value));
}

[[nodiscard]] bool charge_override_table(
    std::span<const MaterialBindingOverrides> overrides,
    const DamagePreviewLimits& limits, std::uint64_t& output_bytes) noexcept {
    std::size_t entries = 0U;
    for (const MaterialBindingOverrides& item : overrides) {
        if (item.properties.size() > limits.material_binding.max_override_properties ||
            item.resources.size() > limits.material_binding.max_override_resources ||
            item.properties.size() > limits.max_material_entries -
                                         std::min(entries, limits.max_material_entries))
            return false;
        entries += item.properties.size();
        if (item.resources.size() > limits.max_material_entries -
                                        std::min(entries, limits.max_material_entries))
            return false;
        entries += item.resources.size();
        std::uint64_t bytes = sizeof(MaterialBindingOverrides);
        const auto add_string = [&](const std::string& value) {
            if (value.size() > limits.max_string_bytes ||
                value.size() > std::numeric_limits<std::uint64_t>::max() - bytes)
                return false;
            bytes += value.size();
            return true;
        };
        if ((item.shader && !add_string(*item.shader)) ||
            (item.blend_mode && !add_string(*item.blend_mode)) ||
            (item.depth_mode && !add_string(*item.depth_mode)) ||
            (item.cull_mode && !add_string(*item.cull_mode)))
            return false;
        for (const auto& [name, value] : item.properties) {
            (void)value;
            if (!add_string(name) ||
                !charge(sizeof(MaterialPropertyOverride), bytes,
                        std::numeric_limits<std::uint64_t>::max()))
                return false;
        }
        for (const auto& [name, value] : item.resources) {
            if (!add_string(name) || !add_string(value.texture) || !add_string(value.file) ||
                !charge(sizeof(MaterialTextureOverride), bytes,
                        std::numeric_limits<std::uint64_t>::max()))
                return false;
        }
        if (!charge(bytes, output_bytes, limits.max_output_bytes)) return false;
    }
    return entries <= limits.max_material_entries;
}

}  // namespace

DamagePreviewResult resolve_damage_preview(
    const DamagePreviewRequest& request, const DamagePreviewLimits& limits) try {
    DamagePreviewResult result;
    if (request.model == nullptr || request.scene == nullptr) {
        fail(result, "DAMAGE_PREVIEW_REQUEST", "Damage preview requires a KN5 model and scene snapshot");
        return result;
    }
    if (limits.max_nodes == 0U || limits.max_materials == 0U ||
        limits.max_selected_roots == 0U || limits.max_descendant_meshes == 0U ||
        limits.max_descendant_work == 0U ||
        limits.max_diagnostics == 0U || limits.max_material_entries == 0U ||
        limits.max_string_bytes == 0U || limits.max_output_bytes == 0U) {
        fail(result, "DAMAGE_PREVIEW_LIMIT", "Damage-preview limits must be nonzero", true);
        return result;
    }
    const auto& model = *request.model;
    const auto& scene = *request.scene;
    if (scene.nodes.size() > limits.max_nodes || model.materials.size() > limits.max_materials ||
        model.materials.size() != scene.materials.size()) {
        fail(result, "DAMAGE_PREVIEW_IDENTITY",
             "Damage-preview model and scene tables do not match their configured limits");
        return result;
    }
    if (!request.base_material_overrides.empty() &&
        request.base_material_overrides.size() != model.materials.size()) {
        fail(result, "DAMAGE_PREVIEW_OVERRIDE_COUNT",
             "Damage-preview base overrides must be empty or material indexed");
        return result;
    }
    std::size_t material_entries = 0U;
    const auto add_entries = [&](std::size_t count) {
        if (count > limits.max_material_entries -
                        std::min(material_entries, limits.max_material_entries))
            return false;
        material_entries += count;
        return true;
    };
    for (const auto& material : model.materials) {
        if (!add_entries(material.properties.size()) ||
            !add_entries(material.resources.size())) {
            fail(result, "DAMAGE_PREVIEW_MATERIAL_ENTRY_LIMIT",
                 "Damage-preview material input exceeds its aggregate entry limit", true);
            return result;
        }
    }
    for (const auto& overrides : request.base_material_overrides) {
        if (!add_entries(overrides.properties.size()) ||
            !add_entries(overrides.resources.size())) {
            fail(result, "DAMAGE_PREVIEW_MATERIAL_ENTRY_LIMIT",
                 "Damage-preview override input exceeds its aggregate entry limit", true);
            return result;
        }
    }

    Kn5SceneNodeMapLimits map_limits = limits.node_map;
    map_limits.max_nodes = std::min(map_limits.max_nodes, limits.max_nodes);
    map_limits.max_work_items = std::min(map_limits.max_work_items, limits.max_nodes);
    map_limits.max_name_bytes = std::min(map_limits.max_name_bytes, limits.max_string_bytes);
    const Kn5SceneNodeMapResult mapped = map_kn5_scene_nodes(model.root, scene, map_limits);
    if (!mapped.ok()) {
        fail(result, mapped.diagnostic.code.empty() ? "DAMAGE_PREVIEW_IDENTITY"
                                                    : mapped.diagnostic.code,
             mapped.diagnostic.message.empty()
                 ? "Damage-preview model and scene identities do not match"
                 : mapped.diagnostic.message,
             mapped.diagnostic.limit_exceeded);
        return result;
    }
    for (std::size_t index = 0U; index < mapped.source_nodes.size(); ++index) {
        const auto& source = *mapped.source_nodes[index];
        if (source.type != 2U && source.type != 3U) continue;
        if (source.materialId >= model.materials.size() ||
            scene.nodes[index].material != source.materialId) {
            fail(result, "DAMAGE_PREVIEW_MATERIAL",
                 "A damage-preview mesh references an unknown or mismatched material");
            return result;
        }
    }

    std::uint64_t output_bytes = 0U;
    std::vector<MaterialBindingOverrides> base;
    if (request.base_material_overrides.empty())
        base.resize(model.materials.size());
    else
        base.assign(request.base_material_overrides.begin(),
                    request.base_material_overrides.end());
    if (!charge_override_table(base, limits, output_bytes)) {
        fail(result, "DAMAGE_PREVIEW_OUTPUT_LIMIT",
             "Damage-preview material overrides exceed their configured limit", true);
        return result;
    }
    result.material_overrides = std::move(base);

    std::vector<MaterialBinding> effective_materials;
    effective_materials.reserve(model.materials.size());
    for (std::size_t index = 0U; index < model.materials.size(); ++index) {
        const auto& material = model.materials[index];
        if (material.name.size() > limits.max_string_bytes ||
            material.shader.size() > limits.max_string_bytes ||
            scene.materials[index].name != material.name ||
            scene.materials[index].shader != material.shader) {
            fail(result, "DAMAGE_PREVIEW_MATERIAL_IDENTITY",
                 "Damage-preview material identity or string data is invalid");
            return result;
        }
        const MaterialBindingOverrides* overrides =
            request.base_material_overrides.empty() ? nullptr
                                                    : &request.base_material_overrides[index];
        effective_materials.push_back(build_material_binding(
            material, false, model.textures.size(), overrides,
            limits.material_binding));
    }

    result.groups.reserve(kPrefixes.size());
    for (const std::string_view prefix : kPrefixes) {
        std::map<std::uint64_t, std::vector<apex::scene::NodeId>> matches;
        for (std::size_t index = 0U; index < mapped.source_nodes.size(); ++index) {
            const auto parsed = suffix_index(mapped.source_nodes[index]->name, prefix);
            if (!parsed.has_value()) continue;
            matches[*parsed].push_back(static_cast<apex::scene::NodeId>(index));
        }

        DamagePrefixGroup group;
        group.prefix = std::string(prefix);
        std::uint64_t wanted = 1U;
        auto found = matches.find(wanted);
        while (found != matches.end()) {
            if (result.selected_roots.size() >= limits.max_selected_roots) {
                fail(result, "DAMAGE_PREVIEW_ROOT_LIMIT",
                     "Damage-preview selected roots exceed their configured limit", true);
                return result;
            }
            const apex::scene::NodeId selected = found->second.front();
            group.selected_roots.push_back(selected);
            result.selected_roots.push_back(selected);
            if (found->second.size() > 1U &&
                !add_diagnostic(result, limits, output_bytes,
                                "DAMAGE_PREVIEW_DUPLICATE_ROOT",
                                "Native F4 uses the first pre-order duplicate damage root",
                                selected))
                return result;
            if (wanted == std::numeric_limits<std::uint64_t>::max()) break;
            ++wanted;
            found = matches.find(wanted);
        }
        group.first_missing = wanted > std::numeric_limits<std::size_t>::max()
                                  ? std::numeric_limits<std::size_t>::max()
                                  : static_cast<std::size_t>(wanted);
        for (const auto& [index, nodes] : matches) {
            if (index > wanted) group.ignored_later_nodes += nodes.size();
        }
        if (group.ignored_later_nodes > 0U &&
            !add_diagnostic(result, limits, output_bytes,
                            "DAMAGE_PREVIEW_NUMBERING_GAP",
                            "Native F4 ignores later damage roots after the first missing suffix"))
            return result;
        if (!charge(sizeof(DamagePrefixGroup) + group.prefix.size() +
                        group.selected_roots.size() * sizeof(apex::scene::NodeId),
                    output_bytes, limits.max_output_bytes)) {
            fail(result, "DAMAGE_PREVIEW_OUTPUT_LIMIT",
                 "Damage-preview prefix output exceeds its byte limit", true);
            return result;
        }
        result.groups.push_back(std::move(group));
    }

    if (!charge(result.selected_roots.size() * sizeof(apex::scene::NodeId),
                output_bytes, limits.max_output_bytes)) {
        fail(result, "DAMAGE_PREVIEW_OUTPUT_LIMIT",
             "Damage-preview selected-root output exceeds its byte limit", true);
        return result;
    }

    std::vector<bool> affected(model.materials.size(), false);
    std::size_t descendant_meshes = 0U;
    std::size_t descendant_work = 0U;
    for (const apex::scene::NodeId root_id : result.selected_roots) {
        std::vector<const apex::formats::Kn5Node*> work = {mapped.source_nodes[root_id]};
        while (!work.empty()) {
            const apex::formats::Kn5Node* node = work.back();
            work.pop_back();
            if (++descendant_work > limits.max_descendant_work) {
                fail(result, "DAMAGE_PREVIEW_WORK_LIMIT",
                     "Damage-preview descendant traversal exceeds its work limit", true);
                return result;
            }
            if (node->type == 2U || node->type == 3U) {
                if (++descendant_meshes > limits.max_descendant_meshes) {
                    fail(result, "DAMAGE_PREVIEW_DESCENDANT_LIMIT",
                         "Damage-preview descendant meshes exceed their configured limit", true);
                    return result;
                }
                if (node->materialId >= model.materials.size()) {
                    fail(result, "DAMAGE_PREVIEW_MATERIAL",
                         "A selected damage descendant references an unknown material");
                    return result;
                }
                affected[node->materialId] = true;
            }
            if (work.size() > limits.max_descendant_work ||
                node->children.size() > limits.max_descendant_work - work.size()) {
                fail(result, "DAMAGE_PREVIEW_WORK_LIMIT",
                     "Damage-preview descendant traversal exceeds its work limit", true);
                return result;
            }
            for (auto child = node->children.rbegin(); child != node->children.rend(); ++child)
                work.push_back(&*child);
        }
    }

    for (std::size_t index = 0U; index < model.materials.size(); ++index) {
        const auto material_id = static_cast<apex::scene::MaterialId>(index);
        const auto& material = model.materials[index];
        const bool has_damage_zones = has_exact_property(material, "damageZones");
        const bool has_glass_damage = has_exact_property(material, "glassDamage");
        if (has_damage_zones) {
            result.damage_zone_materials.push_back(material_id);
            const MaterialBinding& binding = effective_materials[index];
            const bool exact_shader = binding.shader == "ksPerPixelMultiMap_damage_dirt";
            const float dirt = material_scalar(binding, "dirt", 0.0F);
            const bool diffuse_texture = executable_texture(binding, "txDiffuse");
            const bool normal_texture = executable_texture(binding, "txNormal");
            const bool maps_texture = executable_texture(binding, "txMaps");
            const bool damage_texture = executable_texture(binding, "txDamage");
            const bool damage_mask = executable_texture(binding, "txDamageMask");
            const bool exact_resources = diffuse_texture && normal_texture && maps_texture &&
                                         damage_texture && damage_mask;
            const KsPerPixelMaterialResolveResult executable =
                resolve_ks_per_pixel_material_constants(binding);
            if (exact_shader && binding.status == MaterialBindingStatus::complete &&
                dirt == 0.0F && exact_resources && executable.ok()) {
                result.executable_zero_dirt_materials.push_back(material_id);
            } else if (exact_shader && dirt != 0.0F) {
                if (!add_diagnostic(result, limits, output_bytes,
                                    "DAMAGE_PREVIEW_DIRT_UNSUPPORTED",
                                    "The recovered bounded damage stage supports only dirt zero",
                                    apex::scene::invalid_node_id, material_id))
                    return result;
            } else if (exact_shader &&
                       (binding.status != MaterialBindingStatus::complete ||
                        !exact_resources || !executable.ok())) {
                if (!add_diagnostic(result, limits, output_bytes,
                                    "DAMAGE_PREVIEW_STAGE_UNSUPPORTED",
                                    "The bounded damage stage cannot execute this material resource or property set",
                                    apex::scene::invalid_node_id, material_id))
                    return result;
            }
            if (request.broken_visible.has_value()) {
                const float value = *request.broken_visible ? 1.0F : 0.0F;
                if (!charge(sizeof(MaterialPropertyOverride) + sizeof("damageZones"),
                            output_bytes, limits.max_output_bytes)) {
                    fail(result, "DAMAGE_PREVIEW_OUTPUT_LIMIT",
                         "Damage-preview material writes exceed their byte limit", true);
                    return result;
                }
                replace_property(result.material_overrides[index], "damageZones",
                                 MaterialPropertyOverride::vector4_value(
                                     {value, value, value, value}));
            }
        }
        if (affected[index] && has_glass_damage) {
            result.affected_glass_materials.push_back(material_id);
            if (request.broken_visible.has_value()) {
                if (!charge(sizeof(MaterialPropertyOverride) + sizeof("glassDamage"),
                            output_bytes, limits.max_output_bytes)) {
                    fail(result, "DAMAGE_PREVIEW_OUTPUT_LIMIT",
                         "Damage-preview material writes exceed their byte limit", true);
                    return result;
                }
                replace_property(result.material_overrides[index], "glassDamage",
                                 MaterialPropertyOverride::scalar_value(1.0F));
            }
        }
    }

    std::size_t output_entries = 0U;
    for (const auto& overrides : result.material_overrides) {
        if (overrides.properties.size() > limits.max_material_entries -
                                              std::min(output_entries, limits.max_material_entries)) {
            fail(result, "DAMAGE_PREVIEW_MATERIAL_ENTRY_LIMIT",
                 "Damage-preview material output exceeds its aggregate entry limit", true);
            return result;
        }
        output_entries += overrides.properties.size();
        if (overrides.resources.size() > limits.max_material_entries -
                                             std::min(output_entries, limits.max_material_entries)) {
            fail(result, "DAMAGE_PREVIEW_MATERIAL_ENTRY_LIMIT",
                 "Damage-preview material output exceeds its aggregate entry limit", true);
            return result;
        }
        output_entries += overrides.resources.size();
    }

    result.available = !result.selected_roots.empty() ||
                       !result.damage_zone_materials.empty();
    if (request.broken_visible.has_value()) {
        result.activity_overrides.reserve(result.selected_roots.size());
        for (const apex::scene::NodeId node : result.selected_roots)
            result.activity_overrides.push_back({node, *request.broken_visible});
    }
    const std::uint64_t vector_bytes =
        result.activity_overrides.size() * sizeof(apex::scene::NodeActivityOverride) +
        (result.affected_glass_materials.size() + result.damage_zone_materials.size() +
         result.executable_zero_dirt_materials.size()) * sizeof(apex::scene::MaterialId);
    if (!charge(vector_bytes, output_bytes, limits.max_output_bytes)) {
        fail(result, "DAMAGE_PREVIEW_OUTPUT_LIMIT",
             "Damage-preview state output exceeds its byte limit", true);
        return result;
    }
    return result;
} catch (const MaterialBindingError& error) {
    DamagePreviewResult result;
    fail(result, "DAMAGE_PREVIEW_MATERIAL_INPUT",
         std::string("Damage-preview material input is invalid: ") + error.what());
    return result;
} catch (const std::bad_alloc&) {
    DamagePreviewResult result;
    fail(result, "DAMAGE_PREVIEW_ALLOCATION",
         "Bounded damage-preview resolution could not allocate output");
    return result;
}

}  // namespace apex::render
