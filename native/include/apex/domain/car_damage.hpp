#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace apex::domain {

using Vector3 = std::array<float, 3>;

struct DamageExtraEntry { std::string key; std::string value; };

struct DamageSection {
    std::string name;
    std::size_t line = 0;
    std::vector<DamageExtraEntry> extra_entries;
};

struct Scratches : DamageSection { float min_speed = 0.0F; float max_speed = 20.0F; };
struct Oscillations : DamageSection { bool enabled = true; };
struct DamageDefaults : DamageSection { float initial_level = 0.0F; };

struct VisualObject : DamageSection {
    std::uint32_t index = 0;
    std::string object_name;
    Vector3 static_rotation_axis = {0.0F, 0.0F, 0.0F};
    float static_rotation_angle = 0.0F;
    float mult_g = 0.0F;
    std::string damage_zone = "FRONT";
    float min_speed = 0.0F;
    float full_speed = 0.0F;
    Vector3 oscillation_axis = {0.0F, 0.0F, 0.0F};
    float oscillation_min_angle = 0.0F;
    float oscillation_max_angle = 0.0F;
    Vector3 allowed_g = {1.0F, 1.0F, 1.0F};
};

struct ExtraDamageSection { std::string name; std::vector<DamageExtraEntry> entries; };

struct CarDamageConfig {
    std::string source = "data/damage.ini";
    Scratches scratches;
    Oscillations oscillations;
    DamageDefaults damage;
    std::vector<VisualObject> visual_objects;
    std::vector<ExtraDamageSection> extra_sections;
};

struct CarDamageDiagnostic {
    enum class Severity : std::uint8_t { warning, error };
    Severity severity = Severity::warning;
    std::string code;
    std::size_t line = 0;
    std::string message;
};

struct CarDamageLimits {
    std::size_t max_input_bytes = 1U << 20;
    std::size_t max_output_bytes = 1U << 20;
    std::size_t max_sections = 2'048;
    std::size_t max_entries_per_section = 256;
    std::size_t max_extra_entries = 10'000;
    std::size_t max_string_bytes = 4'096;
    std::size_t max_diagnostics = 10'000;
    std::size_t max_visual_objects = 1'024;
};

struct CarDamageParseResult {
    std::optional<CarDamageConfig> config;
    std::vector<CarDamageDiagnostic> diagnostics;
    bool limit_exceeded = false;
};

class CarDamageError final : public std::runtime_error {
public:
    CarDamageError(std::string code, std::string message);
    [[nodiscard]] const std::string& code() const noexcept { return code_; }
private:
    std::string code_;
};

[[nodiscard]] CarDamageParseResult parse_car_damage_ini(
    std::string_view text, std::string source = "data/damage.ini",
    CarDamageLimits limits = {});
[[nodiscard]] std::string serialize_car_damage_ini(
    const CarDamageConfig& config, CarDamageLimits limits = {});

struct CarDamageBaseline { CarDamageConfig config; };
[[nodiscard]] CarDamageBaseline capture_car_damage_baseline(const CarDamageConfig& config);

struct CarDamageEdits {
    std::map<std::string, std::map<std::string, std::string>> values;
};
[[nodiscard]] std::size_t car_damage_edit_count(const CarDamageEdits& edits);
[[nodiscard]] std::size_t apply_car_damage_edits(
    CarDamageConfig& config, const CarDamageEdits& edits,
    const CarDamageBaseline& baseline, CarDamageLimits limits = {});

struct BottomCollider {
    Vector3 centre = {0.0F, 0.0F, 0.0F};
    Vector3 size = {1.0F, 1.0F, 1.0F};
    bool ground_enabled = true;
    // Bounds are optional because the source may omit them. A baseline keeps
    // valid supplied bounds, while edits recompute both values.
    std::optional<Vector3> bounds_min;
    std::optional<Vector3> bounds_max;
};

struct BottomColliderConfig { std::vector<BottomCollider> colliders; };
struct BottomColliderEdit {
    std::optional<Vector3> centre;
    std::optional<Vector3> size;
    std::optional<bool> ground_enabled;
};
using BottomColliderEdits = std::map<std::uint32_t, BottomColliderEdit>;
struct BottomColliderBaseline { BottomColliderConfig config; };

struct BottomColliderLimits {
    std::size_t max_colliders = 1'024;
    std::size_t max_edits = 1'024;
};

struct BottomColliderDiagnostic { std::string code; std::uint32_t index = 0; std::string message; };
struct BottomColliderApplyResult {
    std::size_t applied = 0;
    std::vector<BottomColliderDiagnostic> diagnostics;
};

[[nodiscard]] BottomColliderBaseline capture_bottom_collider_baseline(
    const BottomColliderConfig& config, BottomColliderLimits limits = {});
[[nodiscard]] BottomColliderApplyResult apply_bottom_collider_edits(
    BottomColliderConfig& config, const BottomColliderEdits& edits,
    const BottomColliderBaseline& baseline, BottomColliderLimits limits = {});
[[nodiscard]] std::size_t bottom_collider_edit_count(const BottomColliderEdits& edits);

} // namespace apex::domain
