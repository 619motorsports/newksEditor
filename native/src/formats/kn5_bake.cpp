#include "apex/formats/kn5_bake.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace apex::formats {
namespace {

constexpr std::size_t kMaxElements = 10'000'000;

[[noreturn]] void fail(std::string_view code, std::string message) {
    throw Kn5BakeError(std::string(code), std::move(message));
}

std::string canonical(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return result;
}

void finite(float value, std::string_view label) {
    if (!std::isfinite(value)) fail("non_finite", std::string(label) + " must be finite");
}

template <std::size_t N>
void finiteArray(const std::array<float, N>& values, std::string_view label) {
    for (const auto value : values) finite(value, label);
}

void count(std::size_t value, std::string_view label) {
    if (value > kMaxElements || value > std::numeric_limits<std::uint32_t>::max())
        fail("count_limit", std::string(label) + " count exceeds the KN5 safety limit");
}

void stringValue(std::string_view value, const apex::core::ParseLimits& limits, std::string_view label) {
    if (value.size() > limits.maxStringBytes || value.size() > std::numeric_limits<std::uint32_t>::max())
        fail("string_limit", std::string(label) + " exceeds the configured size limit");
}

void add(std::size_t& total, std::size_t value, std::size_t limit) {
    if (value > std::numeric_limits<std::size_t>::max() - total || value > limit - std::min(limit, total))
        fail("output_limit", "Baked KN5 model exceeds the configured output limit");
    total += value;
}

void appendWarning(std::vector<std::string>& warnings, std::string warning,
                   const apex::core::ParseLimits& limits, std::size_t& warningBytes) {
    stringValue(warning, limits, "bake warning");
    if (warnings.size() == kMaxElements)
        fail("count_limit", "KN5 bake warning count exceeds the safety limit");
    const auto bytes = warning.size() + sizeof(std::string);
    if (bytes > limits.maxOutputBytes || warningBytes > limits.maxOutputBytes - bytes)
        fail("output_limit", "KN5 bake diagnostics exceed the configured output limit");
    warningBytes += bytes;
    warnings.push_back(std::move(warning));
}

std::size_t checkedAdd(std::size_t left, std::size_t right) {
    if (right > std::numeric_limits<std::size_t>::max() - left)
        fail("size_overflow", "Baked KN5 size overflows");
    return left + right;
}

std::size_t checkedMultiply(std::size_t left, std::size_t right) {
    if (left != 0u && right > std::numeric_limits<std::size_t>::max() / left)
        fail("size_overflow", "Baked KN5 size overflows");
    return left * right;
}

void addString(std::size_t& total, std::string_view value, const apex::core::ParseLimits& limits) {
    stringValue(value, limits, "KN5 string");
    add(total, checkedAdd(4u, value.size()), limits.maxOutputBytes);
}

void estimateNode(const Kn5Node& node, std::size_t materialCount, std::size_t& total,
                  const apex::core::ParseLimits& limits, std::size_t depth, std::size_t& nodeCount) {
    if (depth > 1024u) fail("depth_limit", "Scene hierarchy is too deep");
    if (nodeCount == kMaxElements) fail("count_limit", "Scene node count exceeds the KN5 safety limit");
    ++nodeCount;
    if (node.type < 1u || node.type > 3u) fail("invalid_node_type", "KN5 node type is outside 1..3");
    add(total, 9u, limits.maxOutputBytes); // type, child count, active
    addString(total, node.name, limits);
    count(node.children.size(), "child");
    if (node.type == 1u) {
        finiteArray(node.transform, "node transform");
        add(total, 64u, limits.maxOutputBytes);
    } else {
        add(total, 3u, limits.maxOutputBytes);
        if (node.type == 3u) {
            count(node.bones.size(), "bone"); add(total, 4u, limits.maxOutputBytes);
            for (const auto& bone : node.bones) {
                addString(total, bone.name, limits); finiteArray(bone.transform, "bone transform");
                add(total, 64u, limits.maxOutputBytes);
            }
        }
        const std::size_t stride = node.type == 2u ? 11u : 19u;
        if (node.vertexStride != stride || node.vertices.size() % stride != 0u)
            fail("vertex_stride", "KN5 mesh vertex data does not match its stride");
        const auto vertexCount = node.vertices.size() / stride;
        count(vertexCount, "vertex"); count(node.indices.size(), "index");
        for (std::size_t vertexIndex = 0; vertexIndex < node.vertices.size(); ++vertexIndex)
            if (vertexIndex % stride != 8u) finite(node.vertices[vertexIndex], "vertex data");
        for (const auto index : node.indices)
            if (static_cast<std::size_t>(index) >= vertexCount) fail("invalid_index", "KN5 index exceeds vertex count");
        if (static_cast<std::size_t>(node.materialId) >= materialCount)
            fail("invalid_material_index", "KN5 material ID exceeds material count");
        finite(node.lodIn, "LOD in"); finite(node.lodOut, "LOD out");
        const auto vertexBytes = checkedMultiply(checkedMultiply(vertexCount, stride), 4u);
        const auto indexBytes = checkedMultiply(node.indices.size(), 2u);
        const auto payload = checkedAdd(checkedAdd(checkedAdd(4u, vertexBytes), checkedAdd(4u, indexBytes)), 16u);
        add(total, payload, limits.maxOutputBytes);
        if (node.type == 2u) { finiteArray(node.bounds, "bounds"); add(total, 1u, limits.maxOutputBytes); }
    }
    for (const auto& child : node.children) estimateNode(child, materialCount, total, limits, depth + 1u, nodeCount);
}

std::size_t estimate(const Kn5File& model, const apex::core::ParseLimits& limits) {
    if (model.magic != "sc6969") fail("invalid_magic", "Model is not a KN5 file");
    if (model.version < 4u || model.version > 6u) fail("unsupported_version", "Unsupported KN5 version");
    count(model.textures.size(), "texture"); count(model.materials.size(), "material");
    std::size_t total = 6u + 4u + (model.version >= 6u ? 4u : 0u) + 4u;
    if (total > limits.maxOutputBytes) fail("output_limit", "Baked KN5 model exceeds the configured output limit");
    for (const auto& texture : model.textures) {
        stringValue(texture.name, limits, "texture name");
        if (texture.data.size() > std::numeric_limits<std::uint32_t>::max()) fail("texture_size", "Texture data exceeds 32-bit KN5 size");
        add(total, 4u, limits.maxOutputBytes); addString(total, texture.name, limits);
        add(total, checkedAdd(4u, texture.data.size()), limits.maxOutputBytes);
    }
    add(total, 4u, limits.maxOutputBytes);
    for (const auto& material : model.materials) {
        addString(total, material.name, limits); addString(total, material.shader, limits);
        if (material.blendMode > 2u) fail("invalid_blend_mode", "Material blend mode is outside 0..2");
        add(total, 2u + (model.version > 4u ? 4u : 0u) + 4u, limits.maxOutputBytes);
        count(material.properties.size(), "material property");
        for (const auto& property : material.properties) {
            addString(total, property.name, limits); finite(property.value, "property value");
            finiteArray(property.value2, "property value"); finiteArray(property.value3, "property value"); finiteArray(property.value4, "property value");
            add(total, 40u, limits.maxOutputBytes);
        }
        count(material.resources.size(), "material resource"); add(total, 4u, limits.maxOutputBytes);
        for (const auto& resource : material.resources) { addString(total, resource.slot, limits); addString(total, resource.texture, limits); add(total, 4u, limits.maxOutputBytes); }
    }
    std::size_t nodeCount = 0; estimateNode(model.root, model.materials.size(), total, limits, 0, nodeCount);
    return total;
}

template <typename T>
void rejectKeyCollisions(const std::map<std::string, T>& entries, std::string_view label) {
    std::map<std::string, std::string> seen;
    for (const auto& [key, value] : entries) {
        (void)value;
        const auto folded = canonical(key);
        const auto [it, inserted] = seen.emplace(folded, key);
        if (!inserted && it->second != key)
            fail("ambiguous_key", std::string(label) + " contains case-insensitive key collisions");
    }
}

template <typename T>
void rejectNameCollisions(const std::vector<T>& entries, std::string_view label) {
    std::map<std::string, std::string> seen;
    for (const auto& entry : entries) {
        const auto inserted = seen.emplace(canonical(entry.name), entry.name).second;
        if (!inserted)
            fail("ambiguous_key", std::string(label) +
                                      " contains case-insensitive name collisions");
    }
}

void validatePath(std::string_view path) {
    if (path == "root") return;
    if (path.empty()) fail("invalid_path", "Empty hierarchy path is not canonical");
    std::size_t start = 0;
    while (start < path.size()) {
        const auto slash = path.find('/', start);
        const auto part = path.substr(start, slash == std::string_view::npos ? path.size() - start : slash - start);
        if (part.empty() || (part.size() > 1u && part.front() == '0'))
            fail("invalid_path", "Hierarchy path is not canonical");
        for (const auto c : part)
            if (c < '0' || c > '9') fail("invalid_path", "Hierarchy path is not canonical");
        if (slash == std::string_view::npos) break;
        start = slash + 1u;
    }
}

void validateProject(const Kn5BakeProject& project, const apex::core::ParseLimits& limits) {
    count(project.warnings.size(), "project warning");
    for (const auto& warning : project.warnings)
        stringValue(warning, limits, "project warning");
    rejectKeyCollisions(project.materials, "material edits");
    rejectKeyCollisions(project.meshes, "mesh edits");
    rejectKeyCollisions(project.nodes, "node edits");
    rejectKeyCollisions(project.geometry, "geometry edits");
    rejectKeyCollisions(project.baselines, "geometry baselines");
    for (const auto& [path, edit] : project.nodes) {
        validatePath(path); stringValue(path, limits, "node path");
        if (edit.name) stringValue(*edit.name, limits, "node name");
        if (edit.transform) finiteArray(*edit.transform, "node transform");
    }
    for (const auto& [path, edit] : project.geometry) {
        validatePath(path); stringValue(path, limits, "geometry path");
        if (edit.transform) for (const auto value : *edit.transform) finite(value, "geometry transform");
    }
    for (const auto& [path, baseline] : project.baselines) {
        validatePath(path); stringValue(path, limits, "geometry baseline path");
        if (baseline.vertices.size() > kMaxElements * 11u || baseline.indices.size() > kMaxElements * 3u)
            fail("count_limit", "Geometry baseline exceeds the authoring safety limit");
        if (baseline.vertices.size() % 11u != 0u) fail("vertex_stride", "Geometry baseline is not divisible by 11 floats");
        if (baseline.indices.size() % 3u != 0u) fail("topology", "Geometry baseline needs complete triangles");
        const auto baselineVertices = baseline.vertices.size() / 11u;
        for (const auto index : baseline.indices)
            if (static_cast<std::size_t>(index) >= baselineVertices) fail("invalid_index", "Geometry baseline index exceeds vertex count");
        for (std::size_t index = 0; index < baseline.vertices.size(); ++index)
            if (index % 11u != 8u) finite(baseline.vertices[index], "geometry baseline vertex");
    }
    for (const auto& [name, edit] : project.materials) {
        stringValue(name, limits, "material edit name");
        rejectKeyCollisions(edit.properties, "material properties");
        rejectKeyCollisions(edit.resources, "material resources");
        if (edit.shader) stringValue(*edit.shader, limits, "shader");
        for (const auto& [property, values] : edit.properties) {
            stringValue(property, limits, "property name");
            if (values.size() > 4u) fail("property_limit", "Material property has more than four components");
            for (const auto value : values) finite(value, "property value");
        }
        for (const auto& [slot, resource] : edit.resources) {
            stringValue(slot, limits, "resource slot");
            stringValue(resource.texture, limits, "resource texture");
            stringValue(resource.file, limits, "resource file");
            if (resource.color) finiteArray(*resource.color, "resource color");
        }
    }
    for (const auto& [name, edit] : project.meshes) {
        stringValue(name, limits, "mesh edit name");
        if (edit.lod_in && !std::isfinite(*edit.lod_in)) fail("non_finite", "LOD in must be finite");
        if (edit.lod_out && !std::isfinite(*edit.lod_out)) fail("non_finite", "LOD out must be finite");
    }
}

void preflightProjectBudget(const Kn5BakeProject& project, std::size_t sourceBytes,
                            const apex::core::ParseLimits& limits) {
    // This is deliberately conservative: edits may add serialized strings,
    // properties, or resources, so reject a budget-exhausted copy before the
    // source model is copied. The final exact estimate remains authoritative.
    std::size_t budget = sourceBytes;
    const auto reserve = [&budget, &limits](std::size_t bytes) { add(budget, bytes, limits.maxOutputBytes); };
    const auto bytesFor = [](std::size_t left, std::size_t right) {
        return checkedAdd(left, right);
    };
    for (const auto& warning : project.warnings) reserve(bytesFor(warning.size(), 32u));
    for (const auto& [name, edit] : project.materials) {
        reserve(bytesFor(name.size(), 256u));
        if (edit.shader) reserve(bytesFor(edit.shader->size(), 8u));
        for (const auto& [property, values] : edit.properties)
            reserve(bytesFor(bytesFor(property.size(), checkedMultiply(values.size(), sizeof(float))), 32u));
        for (const auto& [slot, resource] : edit.resources)
            reserve(bytesFor(bytesFor(bytesFor(slot.size(), resource.texture.size()), resource.file.size()), 64u));
    }
    for (const auto& [name, edit] : project.meshes) reserve(bytesFor(name.size(), 32u));
    for (const auto& [path, edit] : project.nodes)
        reserve(bytesFor(bytesFor(path.size(), edit.name ? edit.name->size() : 0u), 96u));
    for (const auto& [path, edit] : project.geometry) reserve(bytesFor(path.size(), 32u));
    for (const auto& [path, baseline] : project.baselines) {
        reserve(bytesFor(path.size(), 32u));
        reserve(checkedMultiply(baseline.vertices.size(), sizeof(float)));
        reserve(checkedMultiply(baseline.indices.size(), sizeof(std::uint16_t)));
        if (baseline.bounds) reserve(checkedMultiply(baseline.bounds->size(), sizeof(float)));
    }
}

template <typename T>
const T* findByName(const std::map<std::string, T>& entries, std::string_view name) {
    const auto wanted = canonical(name);
    const auto direct = entries.find(std::string(name));
    if (direct != entries.end()) return &direct->second;
    for (const auto& [key, value] : entries) if (canonical(key) == wanted) return &value;
    return nullptr;
}

Kn5Material* findMaterial(std::vector<Kn5Material>& materials, std::string_view name) {
    const auto wanted = canonical(name);
    for (auto& material : materials) if (canonical(material.name) == wanted) return &material;
    return nullptr;
}

Kn5Node* nodeAtPath(Kn5Node& root, std::string_view path) {
    if (path == "root") return &root;
    Kn5Node* node = &root; std::size_t start = 0;
    while (start < path.size()) {
        const auto slash = path.find('/', start); const auto part = path.substr(start, slash == std::string_view::npos ? path.size() - start : slash - start);
        if (part.empty()) return nullptr;
        std::size_t index = 0;
        for (const auto c : part) {
            if (c < '0' || c > '9') return nullptr;
            const auto digit = static_cast<std::size_t>(c - '0');
            if (index > (std::numeric_limits<std::size_t>::max() - digit) / 10u) return nullptr;
            index = index * 10u + digit;
        }
        if (index >= node->children.size()) return nullptr;
        node = &node->children[index];
        if (slash == std::string_view::npos) break;
        start = slash + 1u;
    }
    return node;
}

void visitMeshes(Kn5Node& node, const std::map<std::string, Kn5BakeMeshEdit>& edits,
                 Kn5BakeApplied& applied, std::vector<std::string>& warnings,
                 const apex::core::ParseLimits& limits, std::size_t& warningBytes) {
    if (node.type == 2u || node.type == 3u) {
        const auto* edit = findByName(edits, node.name);
        if (edit) {
            if (edit->transparent) node.transparent = *edit->transparent;
            if (edit->cast_shadows) node.castShadows = *edit->cast_shadows;
            if (edit->layer) node.layer = *edit->layer;
            if (edit->lod_in) { if (!std::isfinite(*edit->lod_in)) appendWarning(warnings, node.name + ": LOD in is not finite", limits, warningBytes); else node.lodIn = *edit->lod_in; }
            if (edit->lod_out) { if (!std::isfinite(*edit->lod_out)) appendWarning(warnings, node.name + ": LOD out is not finite", limits, warningBytes); else node.lodOut = *edit->lod_out; }
            ++applied.meshes;
        }
    }
    for (auto& child : node.children)
        visitMeshes(child, edits, applied, warnings, limits, warningBytes);
}

} // namespace

Kn5BakeResult bakeKn5(const Kn5File& source, const Kn5BakeProject& project, apex::core::ParseLimits limits) {
    validateProject(project, limits);
    const bool hasEdits = !project.materials.empty() || !project.meshes.empty() || !project.nodes.empty() || !project.geometry.empty();
    if (source.encryption && hasEdits) fail("protected_payload", "CSP-protected KN5 payloads cannot be edited safely");
    const auto sourceBytes = estimate(source, limits);
    if (!project.materials.empty()) {
        rejectNameCollisions(source.materials, "source materials");
        const auto hasResourceEdits = std::any_of(
            project.materials.begin(), project.materials.end(),
            [](const auto& entry) { return !entry.second.resources.empty(); });
        if (hasResourceEdits) rejectNameCollisions(source.textures, "source textures");
    }
    preflightProjectBudget(project, sourceBytes, limits);
    std::size_t warningBytes = 0u;
    for (const auto& warning : project.warnings) {
        const auto bytes = warning.size() + sizeof(std::string);
        if (bytes > limits.maxOutputBytes || warningBytes > limits.maxOutputBytes - bytes)
            fail("output_limit", "KN5 bake diagnostics exceed the configured output limit");
        warningBytes += bytes;
    }
    Kn5BakeResult result{source, {}, project.warnings};

    for (const auto& [editName, edit] : project.materials) {
        auto* material = findMaterial(result.model.materials, editName);
        if (!material) { appendWarning(result.warnings, editName + ": material was not found", limits, warningBytes); continue; }
        bool changed = false;
        if (edit.shader && !edit.shader->empty()) { stringValue(*edit.shader, limits, "shader"); material->shader = *edit.shader; changed = true; }
        if (edit.blend_mode) { if (*edit.blend_mode <= 2u) { material->blendMode = *edit.blend_mode; changed = true; } else appendWarning(result.warnings, editName + ": blendMode is CSP-only and was not baked", limits, warningBytes); }
        if (edit.depth_mode) { material->depthMode = *edit.depth_mode; changed = true; }
        if (edit.cull_mode) appendWarning(result.warnings, editName + ": cullMode is CSP-only and was not baked", limits, warningBytes);
        for (const auto& [name, values] : edit.properties) {
            if (values.empty() || values.size() > 4u) { appendWarning(result.warnings, editName + "." + name + ": property value cannot be stored by KN5", limits, warningBytes); continue; }
            bool valid = true; for (const auto value : values) if (!std::isfinite(value)) valid = false;
            if (!valid) { appendWarning(result.warnings, editName + "." + name + ": property value cannot be stored by KN5", limits, warningBytes); continue; }
            Kn5MaterialProperty* property = nullptr; const auto wanted = canonical(name);
            for (auto& candidate : material->properties) if (canonical(candidate.name) == wanted) { property = &candidate; break; }
            if (!property) { material->properties.push_back({name, 0, {}, {}, {}}); property = &material->properties.back(); }
            property->value = values.size() == 1u ? values[0] : 0.0F;
            property->value2 = {}; property->value3 = {}; property->value4 = {};
            if (values.size() == 2u) std::copy(values.begin(), values.end(), property->value2.begin());
            if (values.size() == 3u) std::copy(values.begin(), values.end(), property->value3.begin());
            if (values.size() == 4u) std::copy(values.begin(), values.end(), property->value4.begin());
            ++result.applied.properties; changed = true;
        }
        for (const auto& [slot, resourceEdit] : edit.resources) {
            // KN5 stores the embedded texture reference.  If both an
            // embedded texture and CSP-only file/color metadata are supplied,
            // the embedded texture is the serializable source of truth.
            if (resourceEdit.texture.empty()) {
                appendWarning(result.warnings, editName + "." + slot + ": external files and solid colors require CSP and were not baked", limits, warningBytes); continue;
            }
            const auto wantedTexture = canonical(resourceEdit.texture); std::string actualTexture;
            for (const auto& texture : result.model.textures) if (canonical(texture.name) == wantedTexture) { actualTexture = texture.name; break; }
            if (actualTexture.empty()) { appendWarning(result.warnings, editName + "." + slot + ": embedded texture " + resourceEdit.texture + " was not found and was not baked", limits, warningBytes); continue; }
            Kn5MaterialResource* resource = nullptr; const auto wantedSlot = canonical(slot);
            for (auto& candidate : material->resources) if (canonical(candidate.slot) == wantedSlot) { resource = &candidate; break; }
            if (!resource) {
                if (!resourceEdit.bind_point) {
                    appendWarning(result.warnings, editName + "." + slot +
                                      ": a new KN5 resource requires an explicit bind point and was not baked",
                                  limits, warningBytes);
                    continue;
                }
                material->resources.push_back({slot, *resourceEdit.bind_point, actualTexture});
                resource = &material->resources.back();
            } else {
                // The serialized integer is the shader bind point. Texture
                // replacement must not rewrite it as a texture-table index.
                resource->texture = actualTexture;
            }
            ++result.applied.resources; changed = true;
        }
        if (changed) ++result.applied.materials;
    }
    visitMeshes(result.model.root, project.meshes, result.applied, result.warnings, limits,
                warningBytes);
    for (const auto& [path, edit] : project.nodes) {
        auto* node = nodeAtPath(result.model.root, path);
        if (!node) { appendWarning(result.warnings, path + ": hierarchy node was not found", limits, warningBytes); continue; }
        bool changed = false;
        if (edit.name && !edit.name->empty()) { stringValue(*edit.name, limits, "node name"); node->name = *edit.name; changed = true; }
        if (edit.active) { node->active = *edit.active; changed = true; }
        if (edit.transform) {
            if (node->type != 1u) appendWarning(result.warnings, path + ": " + node->name + " cannot store a local transform", limits, warningBytes);
            else { finiteArray(*edit.transform, "node transform"); node->transform = *edit.transform; changed = true; }
        }
        if (changed) ++result.applied.nodes;
    }
    std::vector<std::string> geometryWarnings;
    result.applied.geometry = authoring::apply_geometry_edits(
        result.model.root, project.geometry,
        project.geometry.empty() || project.baselines.empty() ? nullptr : &project.baselines,
        &geometryWarnings);
    for (auto& warning : geometryWarnings)
        appendWarning(result.warnings, std::move(warning), limits, warningBytes);
    estimate(result.model, limits);
    return result;
}

} // namespace apex::formats
