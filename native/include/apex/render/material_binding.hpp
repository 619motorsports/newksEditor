#pragma once

#include "apex/render/material_profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace apex::render {

// This is the native-side projection of the material records produced by the
// KN5 reader. It intentionally contains no texture or shader API handles.
struct Kn5MaterialProperty {
    std::string name;
    float value = 0.0F;
    std::array<float, 2> value2{};
    std::array<float, 3> value3{};
    std::array<float, 4> value4{};
};

struct Kn5MaterialResource {
    std::string slot;
    // KN5's serialized integer is the shader resource bind point. It is not
    // an index into the model texture table (txDamageMask is commonly 21).
    std::uint32_t bind_point = 0;
    std::string texture;
};

struct Kn5Material {
    std::string name;
    std::string shader;
    int serialized_blend_mode = 0;
    int depth_mode = 0;
    bool transparent = false;
    std::vector<Kn5MaterialProperty> properties;
    std::vector<Kn5MaterialResource> resources;
};

struct MaterialBindingLimits {
    std::size_t max_properties = 4096;
    std::size_t max_resources = 4096;
    std::size_t max_override_properties = 4096;
    std::size_t max_override_resources = 4096;
    std::size_t max_string_bytes = 1U << 20;
};

enum class MaterialPropertySource : std::uint8_t {
    kn5,
    csp,
    default_value,
};

struct MaterialPropertyValue {
    std::string name;
    float scalar = 0.0F;
    std::array<float, 2> vector2{};
    std::array<float, 3> vector3{};
    std::array<float, 4> vector4{};
    MaterialPropertySource source = MaterialPropertySource::kn5;
};

// CSP values are parsed before they reach this boundary. The kind preserves
// the JS effectiveScalar/effectiveVector2/effectiveVector4 rules without
// making a vector silently look like a scalar.
struct MaterialPropertyOverride {
    enum class Kind : std::uint8_t {
        scalar,
        vector2,
        vector3,
        vector4,
    };

    Kind kind = Kind::scalar;
    float scalar = 0.0F;
    std::array<float, 2> vector2{};
    std::array<float, 3> vector3{};
    std::array<float, 4> vector4{};

    [[nodiscard]] static MaterialPropertyOverride scalar_value(float value) noexcept;
    [[nodiscard]] static MaterialPropertyOverride vector2_value(std::array<float, 2> value) noexcept;
    [[nodiscard]] static MaterialPropertyOverride vector3_value(std::array<float, 3> value) noexcept;
    [[nodiscard]] static MaterialPropertyOverride vector4_value(std::array<float, 4> value) noexcept;
};

enum class MaterialTextureKind : std::uint8_t {
    embedded,
    missing,
    solid_color,
    external_file,
    unresolved_override,
};

struct MaterialTextureOverride {
    // This is a shader bind point, not a model texture-table index.
    std::optional<std::uint32_t> bind_point;
    std::string texture;
    std::string file;
    std::optional<std::array<float, 4>> color;
};

// State overrides use the same textual values as MaterialOverride, while
// property/resource overrides retain their typed values and source identity.
struct MaterialBindingOverrides {
    std::optional<std::string> shader;
    std::optional<std::string> blend_mode;
    std::optional<std::string> depth_mode;
    std::optional<std::string> cull_mode;
    std::optional<bool> is_transparent;
    std::map<std::string, MaterialPropertyOverride> properties;
    std::map<std::string, MaterialTextureOverride> resources;
};

struct MaterialTextureBinding {
    std::string slot;
    MaterialTextureKind kind = MaterialTextureKind::missing;
    std::optional<std::uint32_t> bind_point;
    std::string texture;
    std::string file;
    std::array<float, 4> color{};
    bool required = false;
    bool defaulted = false;
};

enum class MaterialBindingStatus : std::uint8_t {
    complete,
    incomplete,
    unsupported,
};

struct MaterialBindingDiagnostic {
    std::string code;
    std::string field;
    std::string message;
};

struct MaterialBinding {
    std::string name;
    std::string shader;
    MaterialRenderProfile profile;
    std::map<std::string, MaterialPropertyValue> properties;
    std::map<std::string, MaterialTextureBinding> textures;
    MaterialBindingStatus status = MaterialBindingStatus::unsupported;
    // The native resource/state projection is usable by future command
    // encoders. Pixel shader execution remains staged until backend shader
    // modules and validation captures land; this flag must not be interpreted
    // as a rendered approximation.
    bool shader_execution_supported = false;
    std::vector<MaterialBindingDiagnostic> diagnostics;
};

class MaterialBindingError final : public std::runtime_error {
public:
    MaterialBindingError(std::string code, std::string message);

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

// Build a backend-neutral material binding. texture_table_count is retained
// for callers that already have the parsed KN5 table size, but resource bind
// points are deliberately not range-checked against it: KN5 stores shader
// resource slots, not texture-table indices.
[[nodiscard]] MaterialBinding build_material_binding(
    const Kn5Material& material, std::size_t texture_table_count,
    const MaterialBindingOverrides* overrides = nullptr,
    const MaterialBindingLimits& limits = {});

[[nodiscard]] const MaterialPropertyValue* find_material_property(
    const MaterialBinding& binding, std::string_view name) noexcept;
[[nodiscard]] const MaterialTextureBinding* find_material_texture(
    const MaterialBinding& binding, std::string_view slot) noexcept;
[[nodiscard]] float material_scalar(const MaterialBinding& binding,
                                    std::string_view name, float fallback) noexcept;
[[nodiscard]] std::array<float, 2> material_vector2(
    const MaterialBinding& binding, std::string_view name,
    std::array<float, 2> fallback) noexcept;
[[nodiscard]] std::array<float, 4> material_vector4(
    const MaterialBinding& binding, std::string_view name,
    std::array<float, 4> fallback) noexcept;

} // namespace apex::render
