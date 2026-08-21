#include "apex/formats/kn5.hpp"
#include "apex/formats/kn5_write.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace apex::formats;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Kn5Node nodeRoot(std::uint32_t type = 1u) {
    Kn5Node node;
    node.type = type;
    node.kind = type == 1u ? "node" : type == 2u ? "mesh" : "skinnedMesh";
    node.name = "root";
    node.active = true;
    node.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return node;
}

Kn5File synthetic(std::uint32_t version) {
    Kn5File file;
    file.version = version;
    file.sourceMarker = 0x12345678u;
    file.textures.push_back({true, "body.dds", 4u, {1, 2, 3, 4}});
    Kn5Material material;
    material.name = "Body";
    material.shader = "ksPerPixel";
    material.blendMode = 1u;
    material.serializedBlendMode = 1u;
    material.alphaBlend = true;
    material.depthMode = 7u;
    material.properties.push_back({"ksDiffuse", 0.5F, {0.1F, 0.2F}, {0.3F, 0.4F, 0.5F}, {0.6F, 0.7F, 0.8F, 0.9F}});
    material.resources.push_back({"txDiffuse", 21u, "body.dds"});
    file.materials.push_back(material);
    file.root = nodeRoot();
    Kn5Node mesh = nodeRoot(2u);
    mesh.name = "mesh";
    mesh.castShadows = true; mesh.visible = true; mesh.transparent = false;
    mesh.vertices.resize(22u);
    for (std::size_t index = 0; index < mesh.vertices.size(); ++index)
        mesh.vertices[index] = static_cast<float>(index) / 10.0F;
    mesh.indices = {0, 1};
    mesh.materialId = 0u; mesh.layer = 2u; mesh.lodIn = 1.0F; mesh.lodOut = 100.0F;
    mesh.bounds = {0, 0, 0, 1}; mesh.renderable = true;
    Kn5Node skinned = nodeRoot(3u);
    skinned.name = "skin";
    skinned.bones.push_back({"rootBone", nodeRoot().transform});
    skinned.vertices.resize(19u);
    skinned.indices = {0}; skinned.materialId = 0u; skinned.layer = 3u;
    skinned.lodIn = 2.0F; skinned.lodOut = 200.0F;
    file.root.children = {mesh, skinned};
    return file;
}

void writesAllVersionsAndRoundTrips() {
    for (const auto version : {4u, 5u, 6u}) {
        const auto bytes = serializeKn5(synthetic(version));
        const auto parsed = parseKn5(bytes);
        require(parsed.version == version, "version round trip");
        require(parsed.sourceMarker == (version == 6u ? 0x12345678u : 0u), "source marker version rule");
        require(parsed.textures[0].data == std::vector<std::uint8_t>({1, 2, 3, 4}), "texture round trip");
        require(parsed.materials[0].resources[0].textureId == 21u, "resource bind point round trip");
        require(parsed.root.children.size() == 2u && parsed.root.children[1].type == 3u &&
                    parsed.root.children[1].bones.size() == 1u, "static and skinned records");
        require(serializeKn5(parsed) == bytes, "byte-stable synthetic round trip");
    }
}

void rejectsUnsafeModels() {
    auto model = synthetic(6u);
    model.textures[0].data.clear();
    bool caught = false;
    try { (void)serializeKn5(model); } catch (const Kn5WriteError& error) {
        caught = true; require(error.code() == "metadata_only", "metadata-only error code");
    }
    require(caught, "metadata-only texture rejection");

    model = synthetic(6u); model.encryption = Kn5EncryptionInspection{};
    caught = false;
    try { (void)serializeKn5(model); } catch (const Kn5WriteError& error) {
        caught = true; require(error.code() == "protected_payload", "protected payload error code");
    }
    require(caught, "protected payload rejection");

    model = synthetic(6u); model.root.children[0].materialId = 1u;
    caught = false;
    try { (void)serializeKn5(model); } catch (const Kn5WriteError& error) {
        caught = true; require(error.code() == "invalid_material_index", "material index error code");
    }
    require(caught, "invalid material index rejection");

    model = synthetic(6u); model.root.children[0].indices[0] = 2u;
    caught = false;
    try { (void)serializeKn5(model); } catch (const Kn5WriteError& error) {
        caught = true; require(error.code() == "invalid_index", "mesh index error code");
    }
    require(caught, "invalid mesh index rejection");

    model = synthetic(6u); model.root.children[0].vertices[0] = std::numeric_limits<float>::quiet_NaN();
    caught = false;
    try { (void)serializeKn5(model); } catch (const Kn5WriteError& error) {
        caught = true; require(error.code() == "non_finite", "non-finite error code");
    }
    require(caught, "non-finite vertex rejection");

    model = synthetic(6u);
    model.materials[0].blendMode = 0u;
    model.materials[0].serializedBlendMode = 0u;
    model.materials[0].alphaBlend = true;
    caught = false;
    try { (void)serializeKn5(model); } catch (const Kn5WriteError& error) {
        caught = true; require(error.code() == "inconsistent_blend_flags", "blend flag error code");
    }
    require(caught, "inconsistent blend flags rejection");

    model = synthetic(6u);
    model.textures[0].size = 3u;
    caught = false;
    try { (void)serializeKn5(model); } catch (const Kn5WriteError& error) {
        caught = true; require(error.code() == "texture_size_mismatch", "texture size mismatch error code");
    }
    require(caught, "texture size mismatch rejection");

    model = synthetic(6u);
    apex::core::ParseLimits limits; limits.maxOutputBytes = 8u;
    caught = false;
    try { (void)serializeKn5(model, limits); } catch (const Kn5WriteError& error) {
        caught = true; require(error.code() == "output_limit", "output limit error code");
    }
    require(caught, "output size limit rejection");
}

void roundTripsZeroByteTexture() {
    auto model = synthetic(6u);
    model.textures[0].size = 0u;
    model.textures[0].data.clear();
    const auto bytes = serializeKn5(model);
    const auto parsed = parseKn5(bytes);
    require(parsed.textures.size() == 1u && parsed.textures[0].size == 0u &&
                parsed.textures[0].data.empty(),
            "zero-byte texture round trip");
    require(serializeKn5(parsed) == bytes, "zero-byte texture byte stability");
}

void roundTripsRepositoryFixture() {
    for (const auto path : {
             "test/content/cars/619_gen6_arca_base/collider.kn5",
             "test/content/tracks/sepang/sepang.kn5"}) {
        std::ifstream input(path, std::ios::binary);
        if (!input) continue;
        const std::vector<std::uint8_t> source((std::istreambuf_iterator<char>(input)), {});
        const auto parsed = parseKn5(source, path);
        require(serializeKn5(parsed) == source, "repository fixture byte round trip");
    }
}

} // namespace

int main() {
    try {
        writesAllVersionsAndRoundTrips();
        roundTripsZeroByteTexture();
        rejectsUnsafeModels();
        roundTripsRepositoryFixture();
        std::cout << "kn5 write tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "kn5 write tests failed: " << error.what() << '\n';
        return 1;
    }
}
