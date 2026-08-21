#include "apex/formats/kn5_write.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace apex::formats {
namespace {

constexpr std::size_t kMaxElements = 10'000'000;
constexpr std::size_t kMaxDepth = 1024;

[[noreturn]] void fail(std::string_view code, std::string message) {
    throw Kn5WriteError(std::string(code), std::move(message));
}

[[nodiscard]] bool validUtf8(std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<std::uint8_t>(value[index++]);
        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        if (first <= 0x7fu) {
            codePoint = first;
        } else if (first >= 0xc2u && first <= 0xdfu) {
            codePoint = first & 0x1fu;
            continuationCount = 1;
        } else if (first >= 0xe0u && first <= 0xefu) {
            codePoint = first & 0x0fu;
            continuationCount = 2;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            codePoint = first & 0x07u;
            continuationCount = 3;
        } else {
            return false;
        }
        if (continuationCount > value.size() - index) return false;
        for (std::size_t count = 0; count < continuationCount; ++count) {
            const auto byte = static_cast<std::uint8_t>(value[index++]);
            if ((byte & 0xc0u) != 0x80u) return false;
            codePoint = (codePoint << 6u) | (byte & 0x3fu);
        }
        if ((continuationCount == 2 && codePoint < 0x800u) ||
            (continuationCount == 3 && codePoint < 0x10000u) ||
            codePoint > 0x10ffffu || (codePoint >= 0xd800u && codePoint <= 0xdfffu))
            return false;
    }
    return true;
}

[[nodiscard]] std::uint32_t count32(std::size_t count, std::string_view label) {
    if (count > kMaxElements || count > std::numeric_limits<std::uint32_t>::max())
        fail("count_limit", std::string(label) + " count exceeds the KN5 safety limit");
    return static_cast<std::uint32_t>(count);
}

void finite(float value, std::string_view label) {
    if (!std::isfinite(value)) fail("non_finite", std::string(label) + " must be finite");
}

template <std::size_t N>
void finiteArray(const std::array<float, N>& values, std::string_view label) {
    for (const auto value : values) finite(value, label);
}

class Writer final {
public:
    explicit Writer(std::size_t limit) : limit_(limit) { bytes_.reserve(std::min(limit, std::size_t{1024})); }

    void u8(std::uint8_t value) {
        ensure(1);
        bytes_.push_back(value);
    }

    void u32(std::uint32_t value) {
        ensure(4);
        bytes_.push_back(static_cast<std::uint8_t>(value));
        bytes_.push_back(static_cast<std::uint8_t>(value >> 8u));
        bytes_.push_back(static_cast<std::uint8_t>(value >> 16u));
        bytes_.push_back(static_cast<std::uint8_t>(value >> 24u));
    }

    void u16(std::uint16_t value) {
        ensure(2);
        bytes_.push_back(static_cast<std::uint8_t>(value));
        bytes_.push_back(static_cast<std::uint8_t>(value >> 8u));
    }

    void f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }

    void raw(std::span<const std::uint8_t> value) {
        ensure(value.size());
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void string(std::string_view value, std::size_t maxStringBytes) {
        if (value.size() > maxStringBytes || value.size() > std::numeric_limits<std::uint32_t>::max())
            fail("string_limit", "KN5 string exceeds the configured size limit");
        if (!validUtf8(value)) fail("invalid_utf8", "KN5 string is not valid canonical UTF-8");
        u32(static_cast<std::uint32_t>(value.size()));
        raw(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    }

    [[nodiscard]] std::vector<std::uint8_t>&& finish() { return std::move(bytes_); }

private:
    void ensure(std::size_t count) {
        if (count > limit_ - std::min(limit_, bytes_.size()))
            fail("output_limit", "KN5 output exceeds the configured size limit");
        if (count > std::numeric_limits<std::size_t>::max() - bytes_.size())
            fail("size_overflow", "KN5 output size overflows");
    }

    std::vector<std::uint8_t> bytes_;
    std::size_t limit_;
};

void writeFloats(Writer& writer, const std::array<float, 16>& values, std::string_view label) {
    finiteArray(values, label);
    for (const auto value : values) writer.f32(value);
}

template <std::size_t N>
void writeFloats(Writer& writer, const std::array<float, N>& values, std::string_view label) {
    finiteArray(values, label);
    for (const auto value : values) writer.f32(value);
}

void writeMaterial(Writer& writer, const Kn5Material& material, std::uint32_t version,
                   std::size_t maxStringBytes) {
    writer.string(material.name, maxStringBytes);
    writer.string(material.shader, maxStringBytes);
    if (material.blendMode > 2u) fail("invalid_blend_mode", "Material blend mode is outside 0..2");
    if (material.serializedBlendMode > 2u)
        fail("invalid_serialized_blend_mode", "Serialized material blend mode is outside 0..2");
    const bool preserveFlags = material.blendMode == material.serializedBlendMode;
    if (preserveFlags &&
        ((material.blendMode == 0u && (material.alphaBlend || material.alphaToCoverage)) ||
         (material.blendMode == 1u && (!material.alphaBlend || material.alphaToCoverage)) ||
         (material.blendMode == 2u && !material.alphaToCoverage)))
        fail("inconsistent_blend_flags", "Material blend flags do not match the serialized blend mode");
    const bool alphaBlend = preserveFlags ? material.alphaBlend : material.blendMode == 1u;
    const bool alphaToCoverage = preserveFlags ? material.alphaToCoverage : material.blendMode == 2u;
    writer.u8(alphaBlend ? 1u : 0u);
    writer.u8(alphaToCoverage ? 1u : 0u);
    if (version > 4u) writer.u32(material.depthMode);
    writer.u32(count32(material.properties.size(), "material property"));
    for (const auto& property : material.properties) {
        writer.string(property.name, maxStringBytes);
        finite(property.value, property.name + " value");
        writer.f32(property.value);
        writeFloats(writer, property.value2, property.name + " value2");
        writeFloats(writer, property.value3, property.name + " value3");
        writeFloats(writer, property.value4, property.name + " value4");
    }
    writer.u32(count32(material.resources.size(), "material resource"));
    for (const auto& resource : material.resources) {
        writer.string(resource.slot, maxStringBytes);
        // KN5 calls this field a texture ID, but it is the shader resource
        // bind point. It is independent of the model texture table size.
        writer.u32(resource.textureId);
        writer.string(resource.texture, maxStringBytes);
    }
}

void writeNode(Writer& writer, const Kn5Node& node, std::size_t materialCount,
               std::size_t maxStringBytes, std::size_t depth, std::size_t& nodeCount) {
    if (depth > kMaxDepth) fail("depth_limit", "Scene hierarchy is too deep");
    if (nodeCount == kMaxElements) fail("count_limit", "Scene node count exceeds the KN5 safety limit");
    ++nodeCount;
    std::uint32_t type = node.type;
    if (type == 0u) {
        if (node.kind == "node") type = 1u;
        else if (node.kind == "mesh") type = 2u;
        else if (node.kind == "skinnedMesh") type = 3u;
    }
    if (type < 1u || type > 3u) fail("invalid_node_type", "KN5 node type must be 1, 2, or 3");
    writer.u32(type);
    writer.string(node.name, maxStringBytes);
    writer.u32(count32(node.children.size(), "child"));
    writer.u8(node.active ? 1u : 0u);
    if (type == 1u) {
        writeFloats(writer, node.transform, node.name + " transform");
    } else {
        writer.u8(node.castShadows ? 1u : 0u);
        writer.u8(node.visible ? 1u : 0u);
        writer.u8(node.transparent ? 1u : 0u);
        if (type == 3u) {
            writer.u32(count32(node.bones.size(), "bone"));
            for (const auto& bone : node.bones) {
                writer.string(bone.name, maxStringBytes);
                writeFloats(writer, bone.transform, bone.name + " bone transform");
            }
        }
        const std::size_t stride = type == 2u ? 11u : 19u;
        if (node.vertices.size() % stride != 0u)
            fail("vertex_stride", node.name + " vertex data is not divisible by its KN5 stride");
        const auto vertexCount = node.vertices.size() / stride;
        writer.u32(count32(vertexCount, "vertex"));
        for (const auto value : node.vertices) {
            finite(value, node.name + " vertex");
            writer.f32(value);
        }
        writer.u32(count32(node.indices.size(), "index"));
        for (const auto index : node.indices) {
            if (static_cast<std::size_t>(index) >= vertexCount)
                fail("invalid_index", node.name + " index exceeds vertex count");
            writer.u16(index);
        }
        if (static_cast<std::size_t>(node.materialId) >= materialCount)
            fail("invalid_material_index", node.name + " material ID exceeds material count");
        writer.u32(node.materialId);
        writer.u32(node.layer);
        finite(node.lodIn, node.name + " LOD in");
        finite(node.lodOut, node.name + " LOD out");
        writer.f32(node.lodIn);
        writer.f32(node.lodOut);
        if (type == 2u) {
            writeFloats(writer, node.bounds, node.name + " bounds");
            writer.u8(node.renderable ? 1u : 0u);
        }
    }
    for (const auto& child : node.children)
        writeNode(writer, child, materialCount, maxStringBytes, depth + 1u, nodeCount);
}

} // namespace

Kn5WriteError::Kn5WriteError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

std::vector<std::uint8_t> serializeKn5(const Kn5File& file, apex::core::ParseLimits limits) {
    if (file.magic != "sc6969") fail("invalid_magic", "Model is not a KN5 file");
    if (file.version < 4u || file.version > 6u)
        fail("unsupported_version", "KN5 writer supports versions 4, 5, and 6");
    if (file.encryption.has_value())
        fail("protected_payload", "CSP-protected KN5 payloads cannot be rewritten safely");

    Writer writer(limits.maxOutputBytes);
    constexpr std::array<std::uint8_t, 6> magic = {'s', 'c', '6', '9', '6', '9'};
    writer.raw(magic);
    writer.u32(file.version);
    if (file.version >= 6u) writer.u32(file.sourceMarker);

    writer.u32(count32(file.textures.size(), "texture"));
    for (const auto& texture : file.textures) {
        if (texture.data.empty() && texture.size != 0u)
            fail("metadata_only", "Texture " + texture.name + " has no serialized data");
        if (texture.size != texture.data.size())
            fail("texture_size_mismatch", "Texture " + texture.name + " size does not match its serialized data");
        if (texture.data.size() > std::numeric_limits<std::uint32_t>::max())
            fail("texture_size", "Texture data exceeds the KN5 32-bit size field");
        writer.u32(texture.active ? 1u : 0u);
        writer.string(texture.name, limits.maxStringBytes);
        writer.u32(static_cast<std::uint32_t>(texture.data.size()));
        writer.raw(texture.data);
    }
    writer.u32(count32(file.materials.size(), "material"));
    for (const auto& material : file.materials)
        writeMaterial(writer, material, file.version, limits.maxStringBytes);
    std::size_t nodeCount = 0;
    writeNode(writer, file.root, file.materials.size(), limits.maxStringBytes, 0, nodeCount);
    return writer.finish();
}

} // namespace apex::formats
