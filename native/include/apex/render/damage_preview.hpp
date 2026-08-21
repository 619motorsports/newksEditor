#pragma once

#include "apex/render/kn5_scene_node_map.hpp"
#include "apex/render/material_binding.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::render {

struct DamagePreviewLimits {
    std::size_t max_nodes = 1'000'000U;
    std::size_t max_materials = 4096U;
    std::size_t max_selected_roots = 4096U;
    std::size_t max_descendant_meshes = 1'000'000U;
    std::size_t max_descendant_work = 1'000'000U;
    std::size_t max_diagnostics = 100'000U;
    std::size_t max_material_entries = 1'000'000U;
    std::size_t max_string_bytes = 1U << 20;
    std::uint64_t max_output_bytes = 64ULL * 1024ULL * 1024ULL;
    Kn5SceneNodeMapLimits node_map{};
    MaterialBindingLimits material_binding{};
};

struct DamagePreviewRequest {
    const apex::formats::Kn5File* model = nullptr;
    const apex::scene::SceneSnapshot* scene = nullptr;
    // nullopt audits authored state. true and false represent the state after
    // an F4 edge and produce transient activity/material overrides.
    std::optional<bool> broken_visible;
    // Empty or exactly one entry per model material. Damage writes replace
    // only their canonical property names and retain every unrelated field.
    std::span<const MaterialBindingOverrides> base_material_overrides{};
};

struct DamagePrefixGroup {
    std::string prefix;
    std::vector<apex::scene::NodeId> selected_roots;
    std::size_t first_missing = 1U;
    std::size_t ignored_later_nodes = 0U;
};

enum class DamagePreviewDiagnosticSeverity : std::uint8_t {
    warning,
    error,
};

struct DamagePreviewDiagnostic {
    DamagePreviewDiagnosticSeverity severity = DamagePreviewDiagnosticSeverity::warning;
    std::string code;
    std::string message;
    apex::scene::NodeId node = apex::scene::invalid_node_id;
    apex::scene::MaterialId material = apex::scene::invalid_material_id;
};

struct DamagePreviewResult {
    std::vector<DamagePrefixGroup> groups;
    std::vector<apex::scene::NodeId> selected_roots;
    std::vector<apex::scene::MaterialId> affected_glass_materials;
    std::vector<apex::scene::MaterialId> damage_zone_materials;
    // Materials executable by the bounded dirt-zero stage. This does not
    // claim full stock-shader parity for lighting, detail, or reflections.
    std::vector<apex::scene::MaterialId> executable_zero_dirt_materials;
    std::vector<apex::scene::NodeActivityOverride> activity_overrides;
    // Complete material-indexed table, including the supplied base overrides.
    std::vector<MaterialBindingOverrides> material_overrides;
    std::vector<DamagePreviewDiagnostic> diagnostics;
    bool available = false;
    bool supported = true;
    bool limit_exceeded = false;

    [[nodiscard]] bool ok() const noexcept { return supported && !limit_exceeded; }
};

// Resolve the recovered native F4 node sequences and transient material
// writes without mutating the parsed model or immutable SceneSnapshot.
[[nodiscard]] DamagePreviewResult resolve_damage_preview(
    const DamagePreviewRequest& request,
    const DamagePreviewLimits& limits = {});

}  // namespace apex::render
