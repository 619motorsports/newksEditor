#pragma once

#include "apex/core/parse_limits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace apex::formats {

using Kn5Matrix4 = std::array<float, 16>;

struct Kn5Texture {
    bool active = false;
    std::string name;
    std::uint32_t size = 0;
    // Empty when metadataOnly was requested (or when the serialized texture is empty).
    std::vector<std::uint8_t> data;
};

struct Kn5MaterialProperty {
    std::string name;
    float value = 0.0f;
    std::array<float, 2> value2{};
    std::array<float, 3> value3{};
    std::array<float, 4> value4{};
};

struct Kn5MaterialResource {
    std::string slot;
    std::uint32_t textureId = 0;
    std::string texture;
};

struct Kn5Material {
    std::string name;
    std::string shader;
    bool alphaBlend = false;
    bool alphaToCoverage = false;
    std::uint32_t blendMode = 0;
    std::uint32_t serializedBlendMode = 0;
    std::uint32_t depthMode = 0;
    std::vector<Kn5MaterialProperty> properties;
    std::vector<Kn5MaterialResource> resources;
};

struct Kn5Bone {
    std::string name;
    Kn5Matrix4 transform{};
};

struct Kn5Node {
    std::uint32_t type = 0;
    // "node", "mesh", or "skinnedMesh"; retained for parity with the JS model.
    std::string kind;
    std::string name;
    std::vector<Kn5Node> children;
    bool active = false;

    Kn5Matrix4 transform{};

    bool castShadows = false;
    bool visible = false;
    bool transparent = false;
    std::vector<Kn5Bone> bones;
    std::vector<float> vertices;
    std::size_t vertexStride = 0;
    std::vector<std::uint16_t> indices;
    std::uint32_t materialId = 0;
    std::uint32_t layer = 0;
    float lodIn = 0.0f;
    float lodOut = 0.0f;
    std::array<float, 4> bounds{};
    bool renderable = false;
};

struct Kn5EncryptionInspection {
    std::string format = "CSP_KN5ENC_v1";
    bool valid = false;
    std::size_t payloadOffset = 0;
    std::size_t payloadBytes = 0;
    std::size_t recordCount = 0;
    std::vector<std::string> protectedTextures;
    std::vector<std::string> protectedMeshes;
    std::array<std::uint32_t, 2> footer{};
    std::string error;
};

struct Kn5ParseOptions {
    bool metadataOnly = false;
    apex::core::ParseLimits limits{};
};

class Kn5Error final : public std::runtime_error {
public:
    Kn5Error(std::string message, std::size_t offset);

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

private:
    std::size_t offset_;
};

struct Kn5File {
    std::string source;
    std::string magic = "sc6969";
    std::uint32_t version = 0;
    std::uint32_t sourceMarker = 0;
    std::vector<Kn5Texture> textures;
    std::vector<Kn5Material> materials;
    Kn5Node root;
    std::size_t bytesRead = 0;
    std::size_t byteLength = 0;
    std::optional<Kn5EncryptionInspection> encryption;
};

Kn5File parseKn5(std::span<const std::uint8_t> bytes,
                 std::string source = "model.kn5",
                 Kn5ParseOptions options = {});

Kn5File parseKn5(std::span<const std::byte> bytes,
                 std::string source = "model.kn5",
                 Kn5ParseOptions options = {});

// Convenience overload matching callers that only need to omit texture bytes.
Kn5File parseKn5(std::span<const std::uint8_t> bytes, std::string source, bool metadataOnly);

std::optional<Kn5EncryptionInspection> inspectKn5Encryption(
    std::span<const std::uint8_t> bytes, std::size_t payloadOffset = 0);

std::optional<Kn5EncryptionInspection> inspectKn5Encryption(
    std::span<const std::byte> bytes, std::size_t payloadOffset = 0);

struct Kn5WalkEntry {
    const Kn5Node* node = nullptr;
    const Kn5Node* parent = nullptr;
    std::size_t depth = 0;
};

std::vector<Kn5WalkEntry> walkKn5(const Kn5Node& root);

// Returns one visibility value per scene node, in pre-order.
struct Kn5VisibilityEntry {
    const Kn5Node* node = nullptr;
    bool visible = false;
};

std::vector<Kn5VisibilityEntry> computeKn5Visibility(const Kn5Node& root);

// Snake-case aliases are kept for native callers that use the older format APIs.
inline Kn5File parse_kn5(std::span<const std::uint8_t> bytes,
                         std::string source = "model.kn5",
                         Kn5ParseOptions options = {}) {
    return parseKn5(bytes, std::move(source), std::move(options));
}

inline std::optional<Kn5EncryptionInspection> inspect_kn5_encryption(
    std::span<const std::uint8_t> bytes, std::size_t payloadOffset = 0) {
    return inspectKn5Encryption(bytes, payloadOffset);
}

inline std::vector<Kn5WalkEntry> walk_kn5(const Kn5Node& root) { return walkKn5(root); }

}  // namespace apex::formats
