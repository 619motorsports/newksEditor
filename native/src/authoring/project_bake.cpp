#include "apex/authoring/project_bake.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

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
    if (state.nodes.size() > limits.maxNodeEdits)
        fail("node_limit", "project node edit count exceeds the project bake limit");
    if (state.meshes.size() > limits.maxMeshEdits)
        fail("mesh_limit", "project mesh edit count exceeds the project bake limit");
    if (state.geometry.size() > limits.maxGeometryEdits)
        fail("geometry_limit", "project geometry edit count exceeds the project bake limit");
    if (baselines.size() > limits.maxBaselines)
        fail("baseline_limit", "geometry baseline count exceeds the project bake limit");

    std::size_t stringBytes = 0u;
    for (const auto& [path, edit] : state.nodes) {
        validateString(path, limits, "node edit path", stringBytes);
        validatePath(path, "node edit path");
        validateNodeEdit(edit, limits, stringBytes);
    }
    for (const auto& [name, edit] : state.meshes) {
        validateString(name, limits, "mesh edit name", stringBytes);
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

    formats::Kn5BakeProject result;
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
