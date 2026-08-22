#include "apex/authoring/project_bake.hpp"

#include "apex/core/javascript_number.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex::authoring {
namespace {

[[noreturn]] void fail(std::string_view code, std::string_view message) {
    throw ProjectBakeError(std::string(code), std::string(message));
}

void addBounded(std::size_t& total, std::size_t value, std::size_t limit,
                std::string_view label) {
    if (value > limit || total > limit - value)
        fail("output_limit", std::string(label) + " exceeds the project bake limit");
    total += value;
}

std::size_t multiplied(std::size_t left, std::size_t right, std::string_view label) {
    if (left != 0u && right > std::numeric_limits<std::size_t>::max() / left)
        fail("size_overflow", std::string(label) + " size overflows");
    return left * right;
}

void validateString(std::string_view value, const ProjectBakeLimits& limits,
                    std::string_view label, std::size_t& stringBytes) {
    if (value.size() > limits.maxStringBytes)
        fail("string_limit", std::string(label) + " exceeds the project bake string limit");
    addBounded(stringBytes, value.size(), limits.maxTotalStringBytes, "project bake strings");
}

void validateText(std::string_view value, const ProjectBakeLimits& limits,
                  std::string_view label, std::size_t& stringBytes) {
    validateString(value, limits, label, stringBytes);
    if (value.empty()) fail("empty_string", std::string(label) + " must not be empty");
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20u || byte == 0x7fu)
            fail("unsafe_string", std::string(label) + " contains an unsafe character");
    }
}

void validatePath(std::string_view path, std::string_view label) {
    if (path == "root") return;
    if (path.empty()) fail("invalid_path", std::string(label) + " is not canonical");
    std::size_t start = 0u;
    while (start < path.size()) {
        const auto slash = path.find('/', start);
        const auto part = path.substr(
            start, slash == std::string_view::npos ? path.size() - start : slash - start);
        if (part.empty() || (part.size() > 1u && part.front() == '0'))
            fail("invalid_path", std::string(label) + " is not canonical");
        for (const auto character : part) {
            if (character < '0' || character > '9')
                fail("invalid_path", std::string(label) + " is not canonical");
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1u;
    }
}

std::string canonical(std::string_view value) {
    std::string result(value);
    for (auto& character : result) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    }
    return result;
}

template <typename T>
void rejectKeyCollisions(const std::map<std::string, T>& entries,
                         std::string_view label) {
    std::map<std::string, std::string> seen;
    for (const auto& [key, value] : entries) {
        (void)value;
        const auto [position, inserted] = seen.emplace(canonical(key), key);
        if (!inserted && position->second != key)
            fail("ambiguous_key", std::string(label) +
                                      " contains case-insensitive key collisions");
    }
}

std::optional<std::uint32_t> unsignedMode(std::string_view value,
                                          std::uint32_t maximum) {
    const auto number = core::parse_finite_javascript_number(value);
    if (!number || *number < 0.0 || *number > static_cast<double>(maximum) ||
        std::floor(*number) != *number)
        return std::nullopt;
    return static_cast<std::uint32_t>(*number);
}

void appendWarning(formats::Kn5BakeProject& output, std::string warning,
                   const ProjectBakeLimits& limits, std::size_t& stringBytes) {
    validateString(warning, limits, "project bake warning", stringBytes);
    output.warnings.push_back(std::move(warning));
}

const std::string& stringScalar(const MaterialScalar& value, std::string_view field) {
    const auto* text = std::get_if<std::string>(&value);
    if (text == nullptr)
        fail("field_type", std::string(field) + " must be a string");
    return *text;
}

void validateFinite(float value, std::string_view label);

std::vector<float> propertyScalar(const MaterialScalar& value) {
    if (const auto* number = std::get_if<float>(&value)) {
        if (!std::isfinite(*number)) fail("non_finite", "material property must be finite");
        return {*number};
    }
    if (std::holds_alternative<bool>(value))
        fail("field_type", "boolean material properties are not modeled");
    const auto number = core::parse_finite_javascript_number(
        std::get<std::string>(value));
    if (!number) return {};
    const auto converted = static_cast<float>(*number);
    if (!std::isfinite(converted)) return {};
    return {converted};
}

void mapMaterialEdit(std::string_view name, const MaterialEdit& edit,
                     formats::Kn5BakeProject& output, const ProjectBakeLimits& limits,
                     std::size_t& stringBytes, std::size_t& materialFields,
                     std::size_t& materialResources) {
    if (edit.scalars.empty() && edit.vectors.empty() && edit.resources.empty())
        fail("empty_edit", "material edit has no fields");
    addBounded(materialFields, edit.scalars.size(), limits.maxMaterialFields,
               "material fields");
    addBounded(materialFields, edit.vectors.size(), limits.maxMaterialFields,
               "material fields");
    addBounded(materialResources, edit.resources.size(), limits.maxMaterialResources,
               "material resources");
    rejectKeyCollisions(edit.scalars, "material scalar fields");
    rejectKeyCollisions(edit.vectors, "material vector fields");
    rejectKeyCollisions(edit.resources, "material resources");

    std::map<std::string, std::string> properties;
    for (const auto& [field, value] : edit.scalars) {
        (void)value;
        if (field == "shader" || field == "blendMode" || field == "depthMode" ||
            field == "cullMode")
            continue;
        properties.emplace(canonical(field), field);
    }
    for (const auto& [field, value] : edit.vectors) {
        (void)value;
        const auto [position, inserted] = properties.emplace(canonical(field), field);
        if (!inserted)
            fail("ambiguous_key", std::string(name) + "." + position->second +
                                      " has both scalar and vector values");
    }

    formats::Kn5BakeMaterialEdit mapped;
    for (const auto& [field, value] : edit.scalars) {
        validateText(field, limits, "material field", stringBytes);
        if (field == "shader") {
            const auto& text = stringScalar(value, field);
            validateText(text, limits, "material shader", stringBytes);
            mapped.shader = text;
        } else if (field == "blendMode") {
            const auto& text = stringScalar(value, field);
            validateText(text, limits, "material blend mode", stringBytes);
            const auto mode = unsignedMode(text, 255u);
            if (mode)
                mapped.blend_mode = *mode;
            else
                appendWarning(output, std::string(name) + ": blendMode " + text +
                                          " is CSP-only and was not baked",
                              limits, stringBytes);
        } else if (field == "depthMode") {
            const auto& text = stringScalar(value, field);
            validateText(text, limits, "material depth mode", stringBytes);
            const auto mode = unsignedMode(text, std::numeric_limits<std::uint32_t>::max());
            if (mode)
                mapped.depth_mode = *mode;
            else
                appendWarning(output, std::string(name) + ": depthMode " + text +
                                          " is CSP-only and was not baked",
                              limits, stringBytes);
        } else if (field == "cullMode") {
            const auto& text = stringScalar(value, field);
            validateText(text, limits, "material cull mode", stringBytes);
            mapped.cull_mode = text;
        } else {
            if (const auto* text = std::get_if<std::string>(&value))
                validateString(*text, limits, "material property", stringBytes);
            mapped.properties.emplace(field, propertyScalar(value));
        }
    }
    for (const auto& [field, value] : edit.vectors) {
        validateText(field, limits, "material vector field", stringBytes);
        if (value.components < 2u || value.components > value.values.size())
            fail("vector_size", "material vector must contain two to four components");
        for (const auto component : value.values)
            validateFinite(component, "material vector");
        mapped.properties.emplace(
            field, std::vector<float>(value.values.begin(),
                                      value.values.begin() + value.components));
    }
    for (const auto& [slot, value] : edit.resources) {
        validateText(slot, limits, "material resource slot", stringBytes);
        if (value.clear)
            fail("resource_clear", "cleared material resources must not survive project state");
        const auto valueCount = static_cast<unsigned>(value.texture.has_value()) +
                                static_cast<unsigned>(value.file.has_value()) +
                                static_cast<unsigned>(value.color.has_value());
        if (valueCount != 1u)
            fail("resource_value", "material resource must contain exactly one value");
        formats::Kn5BakeResourceEdit resource;
        if (value.texture) {
            validateText(*value.texture, limits, "material texture", stringBytes);
            resource.texture = *value.texture;
        }
        if (value.file) {
            validateText(*value.file, limits, "material resource file", stringBytes);
            resource.file = *value.file;
        }
        if (value.color) {
            for (const auto component : *value.color)
                validateFinite(component, "material resource color");
            resource.color = *value.color;
        }
        mapped.resources.emplace(slot, std::move(resource));
    }
    output.materials.emplace(std::string(name), std::move(mapped));
}

void validateFinite(float value, std::string_view label) {
    if (!std::isfinite(value)) fail("non_finite", std::string(label) + " must be finite");
}

void validateGeometryEdit(const GeometryEdit& edit) {
    if (!edit.transform && !edit.remove_degenerate && !edit.reverse_winding &&
        !edit.recalculate_normals)
        fail("empty_edit", "geometry edit has no fields");
    if (!edit.transform) return;
    for (const auto value : *edit.transform) validateFinite(value, "geometry transform");
}

void validateNodeEdit(const NodeEdit& edit, const ProjectBakeLimits& limits,
                      std::size_t& stringBytes) {
    if (!edit.name && !edit.active && !edit.transform)
        fail("empty_edit", "node edit has no fields");
    if (edit.name) validateText(*edit.name, limits, "node edit name", stringBytes);
    if (edit.transform) {
        for (const auto value : *edit.transform) validateFinite(value, "node transform");
    }
}

void validateMeshEdit(const MeshEdit& edit) {
    if (!edit.transparent && !edit.castShadows && !edit.layer && !edit.lodIn && !edit.lodOut)
        fail("empty_edit", "mesh edit has no fields");
    if (edit.lodIn) validateFinite(*edit.lodIn, "mesh LOD in");
    if (edit.lodOut) validateFinite(*edit.lodOut, "mesh LOD out");
}

void validateBaseline(const GeometryBaseline& baseline, std::string_view path,
                      const ProjectBakeLimits& limits, std::size_t& baselineBytes) {
    if (baseline.vertices.size() % 11u != 0u)
        fail("vertex_stride", std::string(path) + ": baseline vertices are not 11-float records");
    if (baseline.indices.size() % 3u != 0u)
        fail("topology", std::string(path) + ": baseline indices are not complete triangles");
    const auto vertexCount = baseline.vertices.size() / 11u;
    for (std::size_t index = 0u; index < baseline.indices.size(); ++index) {
        if (static_cast<std::size_t>(baseline.indices[index]) >= vertexCount)
            fail("invalid_index", std::string(path) + ": baseline index exceeds vertex count");
    }
    for (std::size_t index = 0u; index < baseline.vertices.size(); ++index) {
        // Tangent slot 8 may be a packed bit pattern and is intentionally not
        // interpreted as a finite float by the geometry authoring contract.
        if (index % 11u != 8u) validateFinite(baseline.vertices[index], "geometry baseline vertex");
    }
    if (baseline.bounds) {
        for (const auto value : *baseline.bounds) validateFinite(value, "geometry baseline bounds");
    }
    addBounded(baselineBytes, multiplied(baseline.vertices.size(), sizeof(float), "baseline vertices"),
               limits.maxBaselineBytes, "geometry baselines");
    addBounded(baselineBytes, multiplied(baseline.indices.size(), sizeof(std::uint16_t), "baseline indices"),
               limits.maxBaselineBytes, "geometry baselines");
    if (baseline.bounds)
        addBounded(baselineBytes, multiplied(baseline.bounds->size(), sizeof(float), "baseline bounds"),
                   limits.maxBaselineBytes, "geometry baselines");
}

} // namespace

formats::Kn5BakeProject buildKn5BakeProject(const ProjectState& state,
                                             const GeometryBaselines& baselines,
                                             ProjectBakeLimits limits) {
    if (state.materials.size() > limits.maxMaterialEdits)
        fail("material_limit", "project material edit count exceeds the project bake limit");
    if (state.nodes.size() > limits.maxNodeEdits)
        fail("node_limit", "project node edit count exceeds the project bake limit");
    if (state.meshes.size() > limits.maxMeshEdits)
        fail("mesh_limit", "project mesh edit count exceeds the project bake limit");
    if (state.geometry.size() > limits.maxGeometryEdits)
        fail("geometry_limit", "project geometry edit count exceeds the project bake limit");
    if (baselines.size() > limits.maxBaselines)
        fail("baseline_limit", "geometry baseline count exceeds the project bake limit");

    rejectKeyCollisions(state.materials, "material edits");
    rejectKeyCollisions(state.meshes, "mesh edits");
    formats::Kn5BakeProject result;
    std::size_t stringBytes = 0u;
    std::size_t materialFields = 0u;
    std::size_t materialResources = 0u;
    for (const auto& [name, edit] : state.materials) {
        validateText(name, limits, "material edit name", stringBytes);
        mapMaterialEdit(name, edit, result, limits, stringBytes, materialFields,
                        materialResources);
    }
    for (const auto& [path, edit] : state.nodes) {
        validateString(path, limits, "node edit path", stringBytes);
        validatePath(path, "node edit path");
        validateNodeEdit(edit, limits, stringBytes);
    }
    for (const auto& [name, edit] : state.meshes) {
        validateText(name, limits, "mesh edit name", stringBytes);
        validateMeshEdit(edit);
    }
    for (const auto& [path, edit] : state.geometry) {
        validateString(path, limits, "geometry edit path", stringBytes);
        validatePath(path, "geometry edit path");
        validateGeometryEdit(edit);
    }
    std::size_t baselineBytes = 0u;
    for (const auto& [path, baseline] : baselines) {
        validateString(path, limits, "geometry baseline path", stringBytes);
        validatePath(path, "geometry baseline path");
        validateBaseline(baseline, path, limits, baselineBytes);
    }

    for (const auto& [path, edit] : state.nodes) {
        formats::Kn5BakeNodeEdit mapped;
        mapped.name = edit.name;
        mapped.active = edit.active;
        mapped.transform = edit.transform;
        result.nodes.emplace(path, std::move(mapped));
    }
    for (const auto& [name, edit] : state.meshes) {
        formats::Kn5BakeMeshEdit mapped;
        mapped.transparent = edit.transparent;
        mapped.cast_shadows = edit.castShadows;
        mapped.layer = edit.layer;
        mapped.lod_in = edit.lodIn;
        mapped.lod_out = edit.lodOut;
        result.meshes.emplace(name, std::move(mapped));
    }
    for (const auto& [path, edit] : state.geometry) result.geometry.emplace(path, edit);
    result.baselines = baselines;
    return result;
}

} // namespace apex::authoring
