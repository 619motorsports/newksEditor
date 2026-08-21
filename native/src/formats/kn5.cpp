#include <apex/formats/kn5.hpp>

#include <apex/core/byte_reader.hpp>
#include <apex/core/parse_error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <sstream>
#include <string_view>
#include <utility>

namespace apex::formats {
namespace {

using apex::core::ByteReader;
using apex::core::ParseError;

constexpr std::size_t kMaxElements = 10'000'000;
constexpr std::size_t kMaxDepth = 1024;
constexpr std::size_t kMinimumTextureBytes = 12;       // active, name length, data length
constexpr std::size_t kMinimumMaterialV4Bytes = 18;   // two names, flags, two counts
constexpr std::size_t kMinimumMaterialV5Bytes = 22;   // v5 adds depth mode
constexpr std::size_t kMinimumPropertyBytes = 44;     // name length + 9 floats
constexpr std::size_t kMinimumResourceBytes = 12;     // slot, texture id, texture name
constexpr std::size_t kMinimumNodeBytes = 13;         // type, name length, children, active
constexpr std::size_t kMinimumBoneBytes = 68;         // name length + 16 floats
constexpr std::size_t kMinimumVertexBytes = 44;       // static vertex: 11 floats
constexpr std::size_t kMinimumSkinnedVertexBytes = 76; // skinned vertex: 19 floats
constexpr std::string_view kEncryptionMarker = "__AC_SHADERS_PATCH_KN5ENC_v1__";

[[noreturn]] void fail(std::string message, std::size_t offset) {
    throw Kn5Error(std::move(message), offset);
}

class NativeAllocationBudget final {
public:
    explicit NativeAllocationBudget(std::size_t limit) : limit_(limit) {}

    void charge(std::size_t bytes, std::size_t offset, std::string_view what) {
        if (bytes > limit_ - used_)
            fail("KN5 native allocation budget exceeded while reserving " + std::string(what), offset);
        used_ += bytes;
    }

    void chargeCount(std::size_t count, std::size_t elementBytes,
                     std::size_t offset, std::string_view what) {
        if (count != 0U && elementBytes > std::numeric_limits<std::size_t>::max() / count)
            fail("KN5 native allocation size overflows while reserving " + std::string(what), offset);
        charge(count * elementBytes, offset, what);
    }

    template <typename T>
    void reserve(std::vector<T>& values, std::size_t count,
                 std::size_t offset, std::string_view what) {
        chargeCount(count, sizeof(T), offset, what);
        values.reserve(count);
    }

private:
    std::size_t limit_ = 0U;
    std::size_t used_ = 0U;
};

[[nodiscard]] std::uint32_t peekU32(const ByteReader& reader, std::size_t offset) noexcept {
    const auto bytes = reader.bytes();
    if (offset > bytes.size() || bytes.size() - offset < 4U) return 0U;
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::string readString(ByteReader& reader, NativeAllocationBudget& budget,
                       std::string_view label) {
    const auto lengthOffset = reader.offset();
    if (reader.remaining() >= 4U) {
        const auto length = peekU32(reader, lengthOffset);
        if (length <= reader.limits().maxStringBytes &&
            static_cast<std::size_t>(length) <= reader.remaining() - 4U) {
            budget.charge(static_cast<std::size_t>(length), lengthOffset, label);
        }
    }
    return reader.string(label);
}

template <typename T>
void checkCount(const ByteReader& reader, T count, std::size_t minimumBytes,
                std::string_view label, std::size_t countOffset) {
    const auto value = static_cast<std::size_t>(count);
    if (value > kMaxElements || (minimumBytes != 0 && value > reader.remaining() / minimumBytes)) {
        fail("Invalid " + std::string(label) + " count " + std::to_string(value), countOffset);
    }
}

template <std::size_t N>
std::array<float, N> readFloats(ByteReader& reader, std::string_view label) {
    std::array<float, N> values{};
    for (auto& value : values) value = reader.f32(label);
    return values;
}

Kn5MaterialProperty readProperty(ByteReader& reader, NativeAllocationBudget& budget) {
    Kn5MaterialProperty property;
    property.name = readString(reader, budget, "property name");
    property.value = reader.f32("property value");
    property.value2 = readFloats<2>(reader, "property vec2 value");
    property.value3 = readFloats<3>(reader, "property vec3 value");
    property.value4 = readFloats<4>(reader, "property vec4 value");
    return property;
}

Kn5Material readMaterial(ByteReader& reader, std::uint32_t version,
                         NativeAllocationBudget& budget) {
    Kn5Material material;
    material.name = readString(reader, budget, "material name");
    material.shader = readString(reader, budget, "shader name");
    material.alphaBlend = reader.u8("alpha-blend flag") != 0;
    material.alphaToCoverage = reader.u8("alpha-to-coverage flag") != 0;
    material.blendMode = material.alphaToCoverage ? 2u : material.alphaBlend ? 1u : 0u;
    material.serializedBlendMode = material.blendMode;
    material.depthMode = version > 4 ? reader.u32("depth mode") : 0;

    const auto propertyOffset = reader.offset();
    const auto propertyCount = reader.u32("material property count");
    checkCount(reader, propertyCount, kMinimumPropertyBytes, "material property", propertyOffset);
    budget.reserve(material.properties, propertyCount, propertyOffset, "material properties");
    for (std::uint32_t index = 0; index < propertyCount; ++index)
        material.properties.push_back(readProperty(reader, budget));

    const auto resourceOffset = reader.offset();
    const auto resourceCount = reader.u32("material resource count");
    checkCount(reader, resourceCount, kMinimumResourceBytes, "material resource", resourceOffset);
    budget.reserve(material.resources, resourceCount, resourceOffset, "material resources");
    for (std::uint32_t index = 0; index < resourceCount; ++index) {
        Kn5MaterialResource resource;
        resource.slot = readString(reader, budget, "resource slot");
        resource.textureId = reader.u32("resource bind point");
        resource.texture = readString(reader, budget, "texture name");
        material.resources.push_back(std::move(resource));
    }
    return material;
}

void validateMeshIndices(const Kn5Node& node, std::size_t vertexCount, std::size_t offset) {
    for (const auto index : node.indices) {
        if (static_cast<std::size_t>(index) >= vertexCount)
            fail("Index " + std::to_string(index) + " exceeds vertex count " +
                     std::to_string(vertexCount),
                 offset);
    }
}

Kn5Node readNode(ByteReader& reader, std::size_t depth, std::size_t materialCount,
                 std::size_t& nodeCount, NativeAllocationBudget& budget) {
    if (depth > kMaxDepth) fail("Scene hierarchy is too deep", reader.offset());
    if (nodeCount == kMaxElements) fail("Too many scene nodes", reader.offset());
    ++nodeCount;
    const auto typeOffset = reader.offset();
    const auto type = reader.u32("node type");
    if (type < 1 || type > 3)
        fail("Unsupported node type " + std::to_string(type), typeOffset);

    Kn5Node node;
    node.type = type;
    node.kind = type == 1 ? "node" : type == 2 ? "mesh" : "skinnedMesh";
    node.name = readString(reader, budget, "node name");
    const auto childOffset = reader.offset();
    const auto childCount = reader.u32("child count");
    checkCount(reader, childCount, kMinimumNodeBytes, "child", childOffset);
    node.active = reader.u8("active flag") != 0;

    std::size_t vertexCount = 0;
    std::size_t meshIndexOffset = reader.offset();
    if (type == 1) {
        node.transform = readFloats<16>(reader, "node transform");
    } else {
        node.castShadows = reader.u8("cast-shadows flag") != 0;
        node.visible = reader.u8("visibility flag") != 0;
        node.transparent = reader.u8("transparent flag") != 0;
        if (type == 3) {
            const auto boneOffset = reader.offset();
            const auto boneCount = reader.u32("bone count");
            checkCount(reader, boneCount, kMinimumBoneBytes, "bone", boneOffset);
            budget.reserve(node.bones, boneCount, boneOffset, "bones");
            for (std::uint32_t index = 0; index < boneCount; ++index) {
                Kn5Bone bone;
                bone.name = readString(reader, budget, "bone name");
                bone.transform = readFloats<16>(reader, "bone transform");
                node.bones.push_back(std::move(bone));
            }
        }
        const auto vertexOffset = reader.offset();
        const auto serializedVertexCount = reader.u32(type == 2 ? "vertex count" : "skinned vertex count");
        const auto vertexMinimum = type == 2 ? kMinimumVertexBytes : kMinimumSkinnedVertexBytes;
        checkCount(reader, serializedVertexCount, vertexMinimum,
                   type == 2 ? "vertex" : "skinned vertex", vertexOffset);
        vertexCount = static_cast<std::size_t>(serializedVertexCount);
        node.vertexStride = type == 2 ? 11 : 19;
        const auto vertexValues = apex::core::checkedMultiply(
            vertexCount, node.vertexStride, "KN5", "vertex data", vertexOffset, "vertex");
        budget.reserve(node.vertices, vertexValues, vertexOffset, "vertex values");
        for (std::size_t index = 0; index < vertexValues; ++index)
            node.vertices.push_back(reader.f32(type == 2 ? "vertex data" : "skinned vertex data"));

        meshIndexOffset = reader.offset();
        const auto indexCount = reader.u32("index count");
        checkCount(reader, indexCount, sizeof(std::uint16_t), "index", meshIndexOffset);
        budget.reserve(node.indices, indexCount, meshIndexOffset, "mesh indices");
        for (std::uint32_t index = 0; index < indexCount; ++index) {
            const auto low = reader.u8("index");
            const auto high = reader.u8("index");
            node.indices.push_back(static_cast<std::uint16_t>(low) |
                                   static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8u));
        }
        validateMeshIndices(node, vertexCount, meshIndexOffset);
        const auto materialOffset = reader.offset();
        node.materialId = reader.u32("material ID");
        if (static_cast<std::size_t>(node.materialId) >= materialCount)
            fail("Material ID " + std::to_string(node.materialId) + " exceeds material count " +
                     std::to_string(materialCount),
                 materialOffset);
        node.layer = reader.u32("layer");
        node.lodIn = reader.f32("LOD in");
        node.lodOut = reader.f32("LOD out");
        if (type == 2) {
            node.bounds = readFloats<4>(reader, "bounding sphere");
            node.renderable = reader.u8("renderable flag") != 0;
        } else {
            node.renderable = true;
        }
    }

    budget.reserve(node.children, childCount, childOffset, "node children");
    for (std::uint32_t index = 0; index < childCount; ++index)
        node.children.push_back(readNode(reader, depth + 1, materialCount, nodeCount, budget));
    return node;
}

void walk(const Kn5Node& node, const Kn5Node* parent, std::size_t depth,
          std::vector<Kn5WalkEntry>& output) {
    output.push_back({&node, parent, depth});
    for (const auto& child : node.children) walk(child, &node, depth + 1, output);
}

void computeVisibility(const Kn5Node& node, bool parentActive,
                       std::vector<Kn5VisibilityEntry>& output) {
    const bool branchActive = parentActive && node.active;
    const bool meshVisible = (node.type != 2 && node.type != 3) || (node.visible && node.renderable);
    output.push_back({&node, branchActive && meshVisible});
    for (const auto& child : node.children) computeVisibility(child, branchActive, output);
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

bool hasMarker(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || kEncryptionMarker.size() > bytes.size() - offset) return false;
    for (std::size_t index = 0; index < kEncryptionMarker.size(); ++index)
        if (bytes[offset + index] != static_cast<std::uint8_t>(kEncryptionMarker[index])) return false;
    return true;
}

void appendProtectedName(std::vector<std::string>& output, std::string_view name,
                         std::size_t start, std::size_t length,
                         NativeAllocationBudget& budget, std::size_t offset,
                         std::string_view label) {
    if (output.size() == output.capacity()) {
        const auto current = output.capacity();
        const auto next = current == 0U ? 1U : current > std::numeric_limits<std::size_t>::max() / 2U
                                                   ? std::numeric_limits<std::size_t>::max()
                                                   : current * 2U;
        budget.reserve(output, next, offset, label);
    }
    budget.charge(length, offset, label);
    output.emplace_back(name.substr(start, length));
}

}  // namespace

Kn5Error::Kn5Error(std::string message, std::size_t offset)
    : std::runtime_error([&] {
          std::ostringstream output;
          output << message << " at byte " << offset;
          return output.str();
      }()),
      offset_(offset) {}

std::optional<Kn5EncryptionInspection> inspectKn5EncryptionWithBudget(
    std::span<const std::uint8_t> bytes, std::size_t payloadOffset,
    NativeAllocationBudget& budget) {
    if (payloadOffset > bytes.size() || bytes.size() < kEncryptionMarker.size() + 8) return std::nullopt;
    const auto first = bytes.size() - kEncryptionMarker.size() - 8;
    const auto lower = std::max(payloadOffset, bytes.size() > 256 ? bytes.size() - 256 : std::size_t{0});
    std::size_t markerOffset = 0;
    bool found = false;
    for (std::size_t offset = first;; --offset) {
        if (offset < lower) break;
        if (hasMarker(bytes, offset)) {
            markerOffset = offset;
            found = true;
            break;
        }
        if (offset == lower) break;
    }
    if (!found || markerOffset < 4 || readU32(bytes, markerOffset - 4) != kEncryptionMarker.size())
        return std::nullopt;
    const auto recordsEnd = markerOffset - 4;
    if (recordsEnd < payloadOffset) return std::nullopt;

    Kn5EncryptionInspection inspection;
    inspection.payloadOffset = payloadOffset;
    inspection.payloadBytes = recordsEnd - payloadOffset;
    std::size_t offset = payloadOffset;
    try {
        while (offset < recordsEnd) {
            if (recordsEnd - offset < 8) fail("truncated encryption record header", offset);
            const auto nameOffset = offset;
            const auto nameLength = readU32(bytes, offset);
            offset += 4;
            if (nameLength == 0 || nameLength > 4096 ||
                static_cast<std::size_t>(nameLength) > recordsEnd - offset - 4)
                fail("invalid encryption record name length " + std::to_string(nameLength), nameOffset);
            const auto nameBytes = bytes.subspan(offset, nameLength);
            budget.charge(static_cast<std::size_t>(nameLength), nameOffset,
                          "encryption record name");
            const auto name = ByteReader::decodeUtf8(nameBytes, "encryption record name", "KN5ENC",
                                                     "KN5", offset);
            offset += nameLength;
            const auto size = readU32(bytes, offset);
            offset += 4;
            if (static_cast<std::size_t>(size) > recordsEnd - offset)
                fail("encryption record " + name + " exceeds payload", offset - 4);
            if (name.size() > 7 && name.rfind("tex.", 0) == 0 && name.size() > 6 &&
                name.substr(name.size() - 2) == ".d")
                appendProtectedName(inspection.protectedTextures, name, 4U,
                                    name.size() - 6U, budget, nameOffset,
                                    "protected texture names");
            if (name.size() > 7 && name.rfind("ver.", 0) == 0 && name.size() > 6 &&
                name.substr(name.size() - 2) == ".x")
                appendProtectedName(inspection.protectedMeshes, name, 4U,
                                    name.size() - 6U, budget, nameOffset,
                                    "protected mesh names");
            offset += size;
            ++inspection.recordCount;
            if (inspection.recordCount > 1'000'000) fail("too many encryption records", nameOffset);
        }
        if (offset != recordsEnd) fail("encryption records do not end at footer", offset);
        inspection.valid = true;
        inspection.footer = {readU32(bytes, markerOffset + kEncryptionMarker.size()),
                             readU32(bytes, markerOffset + kEncryptionMarker.size() + 4)};
    } catch (const Kn5Error& error) {
        inspection.error = error.what();
    } catch (const ParseError& error) {
        inspection.error = error.what();
    }
    return inspection;
}

std::optional<Kn5EncryptionInspection> inspectKn5Encryption(
    std::span<const std::uint8_t> bytes, std::size_t payloadOffset) {
    NativeAllocationBudget budget(default_kn5_native_object_bytes);
    return inspectKn5EncryptionWithBudget(bytes, payloadOffset, budget);
}

std::optional<Kn5EncryptionInspection> inspectKn5Encryption(
    std::span<const std::byte> bytes, std::size_t payloadOffset) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return inspectKn5Encryption(std::span<const std::uint8_t>(data, bytes.size()), payloadOffset);
}

Kn5File parseKn5(std::span<const std::uint8_t> bytes, std::string source,
                 Kn5ParseOptions options) {
    try {
        if (options.maxNativeObjectBytes == 0U)
            throw Kn5Error("KN5 native allocation budget is zero", 0U);
        ByteReader reader(bytes, source, options.limits, "KN5");
        NativeAllocationBudget budget(options.maxNativeObjectBytes);
        std::string magic;
        budget.charge(6U, 0U, "magic");
        magic.reserve(6);
        for (unsigned index = 0; index < 6; ++index) magic.push_back(static_cast<char>(reader.u8("magic")));
        if (magic != "sc6969") fail("Not a KN5 file (magic is " + magic + ")", 0);
        const auto versionOffset = reader.offset();
        const auto version = reader.u32("version");
        if (version < 4 || version > 6) fail("Unsupported KN5 version " + std::to_string(version), versionOffset);
        const auto sourceMarker = version >= 6 ? reader.u32("source marker") : 0u;

        const auto textureOffset = reader.offset();
        const auto textureCount = reader.u32("texture count");
        checkCount(reader, textureCount, kMinimumTextureBytes, "texture", textureOffset);
        Kn5File file;
        budget.charge(source.size(), 0U, "source name");
        file.source = source;
        file.version = version;
        file.sourceMarker = sourceMarker;
        file.byteLength = bytes.size();
        budget.reserve(file.textures, textureCount, textureOffset, "textures");
        for (std::uint32_t index = 0; index < textureCount; ++index) {
            Kn5Texture texture;
            texture.active = reader.u32("texture active flag") != 0;
            texture.name = readString(reader, budget, "texture name");
            const auto sizeOffset = reader.offset();
            texture.size = reader.u32("texture byte size");
            if (static_cast<std::size_t>(texture.size) > reader.remaining())
                fail("Texture data exceeds file", sizeOffset);
            if (!options.metadataOnly) {
                budget.charge(static_cast<std::size_t>(texture.size), sizeOffset,
                              "texture data");
                const auto raw = reader.bytes().subspan(reader.offset(), texture.size);
                texture.data.assign(raw.begin(), raw.end());
            }
            reader.advance(texture.size, "texture data");
            file.textures.push_back(std::move(texture));
        }

        const auto materialOffset = reader.offset();
        const auto materialCount = reader.u32("material count");
        checkCount(reader, materialCount,
                   version > 4 ? kMinimumMaterialV5Bytes : kMinimumMaterialV4Bytes,
                   "material", materialOffset);
        budget.reserve(file.materials, materialCount, materialOffset, "materials");
        for (std::uint32_t index = 0; index < materialCount; ++index)
            file.materials.push_back(readMaterial(reader, version, budget));
        std::size_t nodeCount = 0;
        file.root = readNode(reader, 0, file.materials.size(), nodeCount, budget);
        file.bytesRead = reader.offset();
        if (reader.offset() < bytes.size())
            file.encryption = inspectKn5EncryptionWithBudget(bytes, reader.offset(), budget);
        return file;
    } catch (const Kn5Error&) {
        throw;
    } catch (const ParseError& error) {
        throw Kn5Error(error.what(), error.offset());
    } catch (const std::bad_alloc&) {
        throw Kn5Error("KN5 native allocation failed within the configured budget", 0U);
    }
}

Kn5File parseKn5(std::span<const std::byte> bytes, std::string source,
                 Kn5ParseOptions options) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return parseKn5(std::span<const std::uint8_t>(data, bytes.size()), std::move(source), std::move(options));
}

Kn5File parseKn5(std::span<const std::uint8_t> bytes, std::string source, bool metadataOnly) {
    Kn5ParseOptions options;
    options.metadataOnly = metadataOnly;
    return parseKn5(bytes, std::move(source), std::move(options));
}

std::vector<Kn5WalkEntry> walkKn5(const Kn5Node& root) {
    std::vector<Kn5WalkEntry> result;
    walk(root, nullptr, 0, result);
    return result;
}

std::vector<Kn5VisibilityEntry> computeKn5Visibility(const Kn5Node& root) {
    std::vector<Kn5VisibilityEntry> result;
    computeVisibility(root, true, result);
    return result;
}

}  // namespace apex::formats
