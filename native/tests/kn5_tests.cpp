#include <apex/formats/kn5.hpp>

#include <bit>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <iterator>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void u8(Bytes& bytes, std::uint8_t value) { bytes.push_back(value); }

void u16(Bytes& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
}

void u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void put32At(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    require(offset + 4U <= bytes.size(), "fixture count write is in bounds");
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes[offset + shift / 8U] = static_cast<std::uint8_t>(value >> shift);
}

void f32(Bytes& bytes, float value) { u32(bytes, std::bit_cast<std::uint32_t>(value)); }

void string(Bytes& bytes, std::string_view value) {
    u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void matrix(Bytes& bytes) {
    constexpr float identity[16] = {
        1, 0, 0, 0, 0, 1, 0, 0,
        0, 0, 1, 0, 0, 0, 0, 1};
    for (const auto value : identity) f32(bytes, value);
}

void appendMaterial(Bytes& bytes, std::uint32_t version) {
    string(bytes, "mat");
    string(bytes, "ksPerPixel");
    u8(bytes, 1); // alpha blend
    u8(bytes, 0); // alpha to coverage
    if (version > 4) u32(bytes, 7);
    u32(bytes, 1); // property count
    string(bytes, "ksDiffuse");
    for (unsigned index = 0; index < 10; ++index) f32(bytes, static_cast<float>(index) + 0.5f);
    u32(bytes, 1); // resource count
    string(bytes, "txDiffuse");
    u32(bytes, 0);
    string(bytes, "body.dds");
}

void appendStaticMesh(Bytes& bytes, std::string_view name, std::uint32_t materialId = 0) {
    u32(bytes, 2);
    string(bytes, name);
    u32(bytes, 0); // children
    u8(bytes, 1);  // active
    u8(bytes, 1);  // cast shadows
    u8(bytes, 1);  // visible
    u8(bytes, 0);  // transparent
    u32(bytes, 3); // vertices
    for (unsigned index = 0; index < 33; ++index) f32(bytes, static_cast<float>(index));
    u32(bytes, 3); // indices
    u16(bytes, 0);
    u16(bytes, 1);
    u16(bytes, 2);
    u32(bytes, materialId);
    u32(bytes, 4); // layer
    f32(bytes, 0);
    f32(bytes, 100);
    f32(bytes, 1);
    f32(bytes, 2);
    f32(bytes, 3);
    f32(bytes, 4);
    u8(bytes, 1); // renderable
}

void appendSkinnedMesh(Bytes& bytes, std::string_view name) {
    u32(bytes, 3);
    string(bytes, name);
    u32(bytes, 0); // children
    u8(bytes, 1);  // active
    u8(bytes, 1);  // cast shadows
    u8(bytes, 1);  // visible
    u8(bytes, 0);  // transparent
    u32(bytes, 1); // bones
    string(bytes, "root_bone");
    matrix(bytes);
    u32(bytes, 3); // vertices
    for (unsigned index = 0; index < 57; ++index) f32(bytes, static_cast<float>(index));
    u32(bytes, 3); // indices
    u16(bytes, 0);
    u16(bytes, 1);
    u16(bytes, 2);
    u32(bytes, 0); // material
    u32(bytes, 9); // layer
    f32(bytes, 0);
    f32(bytes, 50);
}

Bytes scene(std::uint32_t version = 6) {
    Bytes bytes;
    bytes.insert(bytes.end(), {'s', 'c', '6', '9', '6', '9'});
    u32(bytes, version);
    if (version >= 6) u32(bytes, 1); // source marker
    u32(bytes, 1); // textures
    u32(bytes, 1); // active
    string(bytes, "body.dds");
    u32(bytes, 3);
    bytes.insert(bytes.end(), {1, 2, 3});
    u32(bytes, 1); // materials
    appendMaterial(bytes, version);
    u32(bytes, 1); // node
    string(bytes, "root");
    u32(bytes, 2); // children
    u8(bytes, 1); // active
    matrix(bytes);
    appendStaticMesh(bytes, "mesh");
    appendSkinnedMesh(bytes, "skin");
    return bytes;
}

void appendEncryptionRecord(Bytes& bytes, std::string_view name, std::span<const std::uint8_t> payload) {
    string(bytes, name);
    u32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
}

Bytes encryptedScene() {
    auto bytes = scene(5);
    const auto payloadOffset = bytes.size();
    const Bytes texturePayload = {1, 2};
    appendEncryptionRecord(bytes, "tex.skin_base.dds.d", texturePayload);
    appendEncryptionRecord(bytes, "ver.GEO_Glass.x", {});
    constexpr std::string_view marker = "__AC_SHADERS_PATCH_KN5ENC_v1__";
    u32(bytes, static_cast<std::uint32_t>(marker.size()));
    bytes.insert(bytes.end(), marker.begin(), marker.end());
    u32(bytes, 0x12345678);
    u32(bytes, 0x9abcdef0);
    require(payloadOffset == scene(5).size(), "encryption payload offset fixture");
    return bytes;
}

template <typename Function>
void requireError(Function&& function, std::string_view context) {
    try {
        function();
    } catch (const apex::formats::Kn5Error&) {
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(context) + ": wrong exception: " + error.what());
    }
    throw std::runtime_error(std::string(context) + ": malformed input was accepted");
}

std::size_t findBytes(const Bytes& bytes, std::string_view value, std::size_t start = 0U) {
    const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(start);
    const auto marker = std::search(begin, bytes.end(), value.begin(), value.end());
    require(marker != bytes.end(), "fixture marker is present");
    return static_cast<std::size_t>(marker - bytes.begin());
}

void setHighCountAndTruncate(Bytes& bytes, std::size_t offset,
                             std::uint32_t count, std::size_t minimumBytes) {
    require(offset + 4U <= bytes.size(), "count field is in bounds");
    put32At(bytes, offset, count);
    const auto required = offset + 4U + static_cast<std::size_t>(count) * minimumBytes;
    if (bytes.size() < required) bytes.resize(required, 0U);
}

void requireNativeBudgetFailure(Bytes bytes, std::string_view context) {
    apex::formats::Kn5ParseOptions options;
    options.maxNativeObjectBytes = 1U * 1024U * 1024U;
    try {
        (void)apex::formats::parseKn5(bytes, "budget.kn5", options);
    } catch (const apex::formats::Kn5Error& error) {
        require(std::string_view(error.what()).find("native allocation budget") != std::string_view::npos,
                context);
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(context) + ": wrong exception: " + error.what());
    }
    throw std::runtime_error(std::string(context) + ": oversized allocation was accepted");
}

void test_v6_scene_and_visibility() {
    const auto bytes = scene();
    const auto parsed = apex::formats::parseKn5(bytes, "synthetic.kn5");
    require(parsed.version == 6 && parsed.sourceMarker == 1, "v6 header");
    require(parsed.textures.size() == 1 && parsed.textures[0].data.size() == 3, "texture data");
    require(parsed.materials.size() == 1 && parsed.materials[0].depthMode == 7, "material metadata");
    require(parsed.materials[0].properties[0].value4[3] == 9.5f, "material vector property");
    require(parsed.materials[0].resources[0].textureId == 0, "resource texture id");
    const auto rows = apex::formats::walkKn5(parsed.root);
    require(rows.size() == 3 && rows[2].node->kind == "skinnedMesh", "recursive node walk");
    require(rows[1].node->vertices.size() == 3 * 11 && rows[1].node->indices.size() == 3,
            "static mesh data");
    require(rows[2].node->bones.size() == 1 && rows[2].node->vertices.size() == 3 * 19,
            "skinned mesh data");
    const auto visibility = apex::formats::computeKn5Visibility(parsed.root);
    require(visibility.size() == 3 && visibility[0].visible && visibility[1].visible && visibility[2].visible,
            "visibility walk");
    require(parsed.bytesRead == parsed.byteLength, "complete scene consumed");
}

void test_v4_v5_headers_and_metadata_only() {
    for (const auto version : {4u, 5u}) {
        const auto parsed = apex::formats::parseKn5(scene(version));
        require(parsed.version == version && parsed.sourceMarker == 0, "v4/v5 header");
        require(parsed.materials[0].depthMode == (version == 5 ? 7u : 0u), "versioned depth mode");
    }
    const auto parsed = apex::formats::parseKn5(scene(), "metadata.kn5", true);
    require(parsed.textures[0].size == 3 && parsed.textures[0].data.empty(), "metadata-only texture parse");
    require(parsed.bytesRead == parsed.byteLength, "metadata-only consumption");
}

void test_encryption_diagnosis() {
    const auto bytes = encryptedScene();
    const auto parsed = apex::formats::parseKn5(bytes);
    require(parsed.bytesRead < parsed.byteLength && parsed.encryption.has_value(), "encryption inspection");
    require(parsed.encryption->valid && parsed.encryption->recordCount == 2, "valid encryption records");
    require(parsed.encryption->protectedTextures[0] == "skin_base.dds", "protected texture name");
    require(parsed.encryption->protectedMeshes[0] == "GEO_Glass", "protected mesh name");
    require(parsed.encryption->payloadOffset == parsed.bytesRead, "protected payload offset");
    auto malformed = bytes;
    malformed[parsed.bytesRead + 4] = 0xff; // corrupt the first record name length
    const auto diagnosis = apex::formats::inspectKn5Encryption(malformed, parsed.bytesRead);
    require(diagnosis.has_value() && !diagnosis->valid, "malformed protected payload diagnosis");
}

void test_every_truncated_prefix() {
    const auto bytes = scene();
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        requireError([&] {
            apex::formats::parseKn5(std::span<const std::uint8_t>(bytes.data(), length));
        }, "truncated prefix " + std::to_string(length));
    }
}

void test_malformed_bounds() {
    auto badType = scene();
    // The first root type starts after the complete header, texture, and material.
    const auto parsed = apex::formats::parseKn5(scene());
    const auto rootType = parsed.root.type; // keep this test independent of serialized offsets
    require(rootType == 1, "root type fixture");
    // Locate root type by parsing the known fixture and searching its little-endian value.
    const Bytes nameBytes = {'r', 'o', 'o', 't'};
    const auto marker = std::search(badType.begin(), badType.end(), nameBytes.begin(), nameBytes.end());
    require(marker != badType.end() && marker - badType.begin() >= 4, "root marker fixture");
    const auto rootNameOffset = static_cast<std::size_t>(marker - badType.begin());
    const auto rootTypeOffset = rootNameOffset - 4;
    badType[rootTypeOffset] = 9;
    requireError([&] { apex::formats::parseKn5(badType); }, "invalid node type");

    auto badIndex = scene();
    // The synthetic static mesh index data is immediately followed by material ID.
    const Bytes meshName = {'m', 'e', 's', 'h'};
    const auto meshMarker = std::search(badIndex.begin(), badIndex.end(), meshName.begin(), meshName.end());
    require(meshMarker != badIndex.end(), "mesh marker fixture");
    const auto meshNameOffset = static_cast<std::size_t>(meshMarker - badIndex.begin());
    const auto meshTypeOffset = meshNameOffset - 4;
    // Replace the first static index (after the fixed static record prefix) by 99.
    // Search for the known 3-index sequence after the mesh record.
    const auto sequence = Bytes{0, 0, 1, 0, 2, 0};
    const auto indexMarker = std::search(badIndex.begin() + static_cast<std::ptrdiff_t>(meshTypeOffset),
                                         badIndex.end(), sequence.begin(), sequence.end());
    require(indexMarker != badIndex.end(), "index marker fixture");
    const auto indexOffset = static_cast<std::size_t>(indexMarker - badIndex.begin());
    badIndex[indexOffset] = 99;
    requireError([&] { apex::formats::parseKn5(badIndex); }, "index bound");

    auto hugeCount = scene();
    // Texture count is directly after the v6 source marker.
    hugeCount[14] = 0xff;
    hugeCount[15] = 0xff;
    hugeCount[16] = 0xff;
    hugeCount[17] = 0xff;
    requireError([&] { apex::formats::parseKn5(hugeCount); }, "texture count bound");
    (void)meshTypeOffset;
}

void test_native_allocation_budget() {
    {
        apex::formats::Kn5ParseOptions options;
        options.maxNativeObjectBytes = 0U;
        bool caught = false;
        try {
            (void)apex::formats::parseKn5(scene(), "zero-budget.kn5", options);
        } catch (const apex::formats::Kn5Error& error) {
            caught = true;
            require(std::string_view(error.what()).find("native allocation budget") != std::string_view::npos,
                    "zero native allocation budget diagnostic");
        }
        require(caught, "zero native allocation budget is rejected");
    }
    // Each case leaves enough serialized padding for checkCount() to accept
    // the hostile count. The parser must reject before reserve() allocates the
    // corresponding native vector. The zero padding is intentionally a
    // truncated record tail, not a valid KN5 container.
    {
        auto bytes = scene();
        setHighCountAndTruncate(bytes, 14U, 100'000U, 12U);
        requireNativeBudgetFailure(std::move(bytes), "texture reserve budget");
    }
    {
        auto bytes = scene();
        setHighCountAndTruncate(bytes, 41U, 100'000U, 22U);
        requireNativeBudgetFailure(std::move(bytes), "material reserve budget");
    }
    {
        auto bytes = scene();
        const auto marker = findBytes(bytes, "ksDiffuse");
        setHighCountAndTruncate(bytes, marker - 8U, 100'000U, 44U);
        requireNativeBudgetFailure(std::move(bytes), "property reserve budget");
    }
    {
        auto bytes = scene();
        const auto marker = findBytes(bytes, "txDiffuse");
        setHighCountAndTruncate(bytes, marker - 8U, 100'000U, 12U);
        requireNativeBudgetFailure(std::move(bytes), "resource reserve budget");
    }
    {
        auto bytes = scene();
        const auto marker = findBytes(bytes, "skin");
        setHighCountAndTruncate(bytes, marker + 12U, 100'000U, 68U);
        requireNativeBudgetFailure(std::move(bytes), "bone reserve budget");
    }
    {
        auto bytes = scene();
        const auto marker = findBytes(bytes, "mesh");
        setHighCountAndTruncate(bytes, marker + 12U, 1'000'000U, 44U);
        requireNativeBudgetFailure(std::move(bytes), "vertex reserve budget");
    }
    {
        auto bytes = scene();
        const auto marker = findBytes(bytes, "mesh");
        // The first index starts immediately after the current three vertices.
        setHighCountAndTruncate(bytes, marker + 12U + 4U + 3U * 44U,
                                1'000'000U, sizeof(std::uint16_t));
        requireNativeBudgetFailure(std::move(bytes), "index reserve budget");
    }
    {
        auto bytes = scene();
        const auto marker = findBytes(bytes, "root");
        setHighCountAndTruncate(bytes, marker + 4U, 1'000'000U, 13U);
        requireNativeBudgetFailure(std::move(bytes), "child reserve budget");
    }
    {
        auto bytes = scene();
        // The texture payload begins after the name and serialized size.
        put32At(bytes, 34U, 2U * 1024U * 1024U);
        bytes.resize(38U + 2U * 1024U * 1024U, 0U);
        requireNativeBudgetFailure(std::move(bytes), "texture payload budget");
    }
    {
        auto bytes = scene(5U);
        for (std::size_t index = 0U; index < 20'000U; ++index)
            appendEncryptionRecord(bytes, "tex.skin.dds.d", {});
        constexpr std::string_view marker = "__AC_SHADERS_PATCH_KN5ENC_v1__";
        u32(bytes, static_cast<std::uint32_t>(marker.size()));
        bytes.insert(bytes.end(), marker.begin(), marker.end());
        u32(bytes, 0U);
        u32(bytes, 0U);
        apex::formats::Kn5ParseOptions options;
        options.maxNativeObjectBytes = 1U * 1024U * 1024U;
        const auto parsed = apex::formats::parseKn5(std::move(bytes), "encryption-budget.kn5", options);
        require(parsed.encryption.has_value() && !parsed.encryption->valid &&
                    parsed.encryption->error.find("native allocation budget") != std::string::npos,
                "encryption vector budget");
    }
}

bool readFile(const std::string& path, Bytes& output) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto end = file.tellg();
    if (end < 0) return false;
    output.resize(static_cast<std::size_t>(end));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    return file.good() || file.eof();
}

void test_repository_fixtures() {
    // CTest normally runs from the build directory, while local invocations often run
    // from the repository. Accept both layouts and keep this check optional for package
    // builds that intentionally omit binary fixtures.
    const std::array<std::string, 3> roots = {"test/content", "../test/content", "../../test/content"};
    Bytes collider;
    for (const auto& root : roots)
        if (readFile(root + "/cars/619_gen6_arca_base/collider.kn5", collider)) break;
    if (!collider.empty()) {
        const auto parsed = apex::formats::parseKn5(collider, "collider.kn5", true);
        require(parsed.version == 6 && parsed.materials.size() == 1, "repository collider metadata");
        require(parsed.root.name == "FBX: collider.fbx", "repository collider root");
        require(apex::formats::walkKn5(parsed.root).size() == 3, "repository collider nodes");
        require(parsed.bytesRead == parsed.byteLength, "repository collider consumption");
    }

    Bytes track;
    for (const auto& root : roots)
        if (readFile(root + "/tracks/sepang/sepang.kn5", track)) break;
    if (!track.empty()) {
        const auto parsed = apex::formats::parseKn5(track, "sepang.kn5", true);
        require(parsed.version == 5 && parsed.sourceMarker == 0, "repository track header");
        require(parsed.textures.size() == 130 && parsed.materials.size() == 167,
                "repository track metadata");
        require(parsed.bytesRead == parsed.byteLength, "repository track consumption");
    }
}

}  // namespace

int main() {
    try {
        test_v6_scene_and_visibility();
        test_v4_v5_headers_and_metadata_only();
        test_encryption_diagnosis();
        test_every_truncated_prefix();
        test_malformed_bounds();
        test_native_allocation_budget();
        test_repository_fixtures();
        std::cout << "KN5 tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "KN5 tests failed: " << error.what() << '\n';
        return 1;
    }
}
