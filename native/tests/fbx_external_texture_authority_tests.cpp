#include "apex/render/fbx_external_texture_authority.hpp"

#include "apex/formats/acd.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using apex::assets::AssetSource;
using apex::formats::AcdArchive;
using apex::formats::AcdEntry;
using apex::formats::FbxMaterialFileTextureCandidate;
using apex::formats::FbxSceneConversion;
using apex::render::ExternalTextureAuthorityStatus;
using apex::render::FbxExternalTextureAuthorityLimits;
using apex::render::invalid_fbx_texture_selection;
using apex::render::resolve_fbx_external_texture_authority;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset,
           std::uint32_t value) {
    require(offset + 4U <= bytes.size(), "DDS fixture write is in bounds");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

std::vector<std::uint8_t> rgba8Dds(std::array<std::uint8_t, 4U> pixel) {
    std::vector<std::uint8_t> bytes(152U, 0U);
    put32(bytes, 0U, 0x20534444U);
    put32(bytes, 4U, 124U);
    put32(bytes, 12U, 1U);
    put32(bytes, 16U, 1U);
    put32(bytes, 20U, 4U);
    put32(bytes, 28U, 1U);
    put32(bytes, 76U, 32U);
    put32(bytes, 80U, 0x40U);
    put32(bytes, 88U, 32U);
    put32(bytes, 92U, 0x000000ffU);
    put32(bytes, 96U, 0x0000ff00U);
    put32(bytes, 100U, 0x00ff0000U);
    put32(bytes, 104U, 0xff000000U);
    for (std::size_t index = 0U; index < pixel.size(); ++index)
        bytes[128U + index] = pixel[index];
    return bytes;
}

AcdEntry entry(std::string path, std::vector<std::uint8_t> bytes) {
    AcdEntry result;
    result.name = path;
    result.path = std::move(path);
    result.safe = true;
    result.size = bytes.size();
    result.data = std::move(bytes);
    return result;
}

AssetSource sourceWith(std::vector<AcdEntry> entries) {
    AcdArchive archive;
    archive.source = "fixture/data.acd";
    archive.assetName = "fixture";
    archive.entries = std::move(entries);
    AssetSource source;
    source.addAcdArchive(std::move(archive));
    return source;
}

FbxSceneConversion conversion(std::size_t materials = 1U) {
    FbxSceneConversion result;
    for (std::size_t index = 0U; index < materials; ++index)
        result.snapshot.materials.push_back(
            {"Material" + std::to_string(index), "Phong",
             apex::scene::BlendMode::opaque});
    return result;
}

void addCandidate(FbxSceneConversion& value, std::size_t material,
                  std::int64_t object, std::string channel,
                  std::string basename, std::size_t connection) {
    value.file_texture_candidates.push_back(
        FbxMaterialFileTextureCandidate{
            static_cast<apex::scene::MaterialId>(material), object,
            std::move(channel), std::move(basename), connection});
}

std::filesystem::path testRoot(std::string_view suffix) {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("apex-fbx-texture-authority-" + std::string(suffix) + "-" +
            std::to_string(static_cast<long long>(stamp)));
}

void writeBytes(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot create texture fixture");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("cannot write texture fixture");
}

void selectsFirstExistingCandidateInNativeOrder() {
    auto value = conversion();
    // Deliberately reverse raw storage. Native channel rank must still put the
    // missing diffuse candidate before the existing ambient candidate.
    addCandidate(value, 0U, 402, "SpecularColor", "specular.dds", 7U);
    addCandidate(value, 0U, 400, "AmbientColor", "ambient.dds", 5U);
    addCandidate(value, 0U, 401, "DiffuseColor", "missing.dds", 9U);
    auto source = sourceWith({
        entry("textures/ambient.dds", rgba8Dds({10U, 20U, 30U, 255U})),
        entry("textures/specular.dds", rgba8Dds({40U, 50U, 60U, 255U}))});

    const auto result = resolve_fbx_external_texture_authority(
        value, "fbx-root", source);
    require(result.ok() && result.selections.size() == 1U &&
                result.selections.front().texture_object_id == 400 &&
                result.selections.front().channel == "AmbientColor" &&
                result.material_selection_indices ==
                    std::vector<std::size_t>{0U},
            "first existing candidate follows native channel order");
    require(result.authority.resources.size() == 1U &&
                result.authority.resources.front()
                        .plan.levels.front().pixels[0] == 10U,
            "selected candidate is decoded by the shared authority");
}

void keepsFirstExistingSourceAndLeavesMissingNull() {
    auto value = conversion(2U);
    addCandidate(value, 0U, 400, "DiffuseColor", "first.dds", 1U);
    addCandidate(value, 0U, 401, "DiffuseColor", "later.dds", 2U);
    addCandidate(value, 1U, 402, "DiffuseColor", "absent.dds", 3U);
    auto source = sourceWith({
        entry("first.dds", rgba8Dds({1U, 2U, 3U, 255U})),
        entry("later.dds", rgba8Dds({9U, 8U, 7U, 255U}))});

    const auto result = resolve_fbx_external_texture_authority(
        value, "fbx-root", source);
    require(result.ok() && result.selections.size() == 1U &&
                result.selections.front().texture_object_id == 400,
            "later file cannot replace the first existing source");
    require(result.material_selection_indices[1] ==
                invalid_fbx_texture_selection &&
                result.diagnostics.size() == 1U &&
                result.diagnostics.front().code ==
                    "fbx_texture_material_missing",
            "all-missing material retains a diagnosed null resource");
}

void embeddedContentShadowsExternalLookup() {
    auto value = conversion();
    addCandidate(value, 0U, 400, "DiffuseColor", "duplicate.dds", 1U);
    value.embedded_images.push_back(
        {500, "embedded.dds", rgba8Dds({1U, 2U, 3U, 255U})});
    value.embedded_texture_candidates.push_back(
        {0U, 400, 500, 0U, "DiffuseColor", 1U});
    auto source = sourceWith({
        entry("a/duplicate.dds", rgba8Dds({4U, 5U, 6U, 255U})),
        entry("b/duplicate.dds", rgba8Dds({7U, 8U, 9U, 255U}))});

    const auto result = resolve_fbx_external_texture_authority(
        value, "fbx-root", source);
    require(result.ok() && result.selections.empty() &&
                result.authority.resources.empty() &&
                result.material_selection_indices ==
                    std::vector<std::size_t>{invalid_fbx_texture_selection},
            "nonempty embedded content shadows external ambiguity");

    value.embedded_texture_candidates[0].embedded_image_index = 9U;
    const auto malformed = resolve_fbx_external_texture_authority(
        value, "fbx-root", source);
    require(malformed.status ==
                ExternalTextureAuthorityStatus::invalid_request &&
                malformed.selections.empty(),
            "malformed embedded shadow identity fails closed");
}

void rejectsAmbiguityBeforeLaterCandidates() {
    auto value = conversion();
    addCandidate(value, 0U, 400, "DiffuseColor", "duplicate.dds", 1U);
    addCandidate(value, 0U, 401, "AmbientColor", "ambient.dds", 2U);
    auto source = sourceWith({
        entry("a/duplicate.dds", rgba8Dds({1U, 2U, 3U, 255U})),
        entry("b/duplicate.dds", rgba8Dds({4U, 5U, 6U, 255U})),
        entry("ambient.dds", rgba8Dds({7U, 8U, 9U, 255U}))});

    const auto result = resolve_fbx_external_texture_authority(
        value, "fbx-root", source);
    require(result.status == ExternalTextureAuthorityStatus::ambiguous &&
                result.selections.empty() && result.authority.resources.empty() &&
                !result.diagnostics.empty() &&
                result.diagnostics.front().candidate_index == 0U,
            "ambiguous earlier candidate fails atomically");
}

void rejectsMalformedCandidatesAndPayloads() {
    const std::array<std::string, 10U> unsafe = {
        "",          ".",          "..",       "a/b.dds",
        "a\\b.dds", "C:body.dds", "x:y.dds",  std::string("bad\0.dds", 8U),
        std::string("bad\x1f.dds", 8U), std::string("bad\x7f.dds", 8U)};
    for (const auto& basename : unsafe) {
        auto value = conversion();
        addCandidate(value, 0U, 400, "DiffuseColor", basename, 1U);
        auto source = sourceWith({});
        const auto result = resolve_fbx_external_texture_authority(
            value, "fbx-root", source);
        require(result.status == ExternalTextureAuthorityStatus::rejected &&
                    result.selections.empty() &&
                    result.authority.resources.empty(),
                "unsafe basename fails before authority work");
    }

    auto wrongChannel = conversion();
    addCandidate(wrongChannel, 0U, 400, "NormalMap", "normal.dds", 1U);
    auto wrongChannelSource = sourceWith({});
    const auto channelResult = resolve_fbx_external_texture_authority(
        wrongChannel, "fbx-root", wrongChannelSource);
    require(channelResult.status ==
                ExternalTextureAuthorityStatus::invalid_request &&
                channelResult.selections.empty(),
            "non-native channel is rejected");

    auto malformed = conversion();
    addCandidate(malformed, 0U, 400, "DiffuseColor", "broken.dds", 1U);
    auto malformedSource = sourceWith(
        {entry("broken.dds", std::vector<std::uint8_t>{'D', 'D', 'S'})});
    const auto malformedResult = resolve_fbx_external_texture_authority(
        malformed, "fbx-root", malformedSource);
    require(!malformedResult.ok() && malformedResult.selections.empty() &&
                malformedResult.authority.resources.empty() &&
                !malformedResult.authority.diagnostics.empty(),
            "truncated image fails atomically in the shared decoder");
}

void enforcesCandidateAndMetadataLimits() {
    auto value = conversion();
    addCandidate(value, 0U, 400, "DiffuseColor", "a.dds", 1U);
    addCandidate(value, 0U, 401, "AmbientColor", "b.dds", 2U);
    auto source = sourceWith({});

    auto limits = FbxExternalTextureAuthorityLimits{};
    limits.max_candidates = 1U;
    auto result = resolve_fbx_external_texture_authority(
        value, "fbx-root", source, limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit &&
                result.selections.empty(),
            "aggregate candidate limit fails before lookup");

    limits = FbxExternalTextureAuthorityLimits{};
    limits.max_candidates_per_material = 1U;
    result = resolve_fbx_external_texture_authority(
        value, "fbx-root", source, limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit &&
                result.selections.empty(),
            "per-material candidate limit fails atomically");

    limits = FbxExternalTextureAuthorityLimits{};
    limits.max_metadata_bytes = 1U;
    result = resolve_fbx_external_texture_authority(
        value, "fbx-root", source, limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit &&
                result.selections.empty(),
            "metadata limit fails before lookup");
}

void retainsOwnedBytesAndDirectoryPrecedence() {
    const auto root = testRoot("ownership");
    std::filesystem::create_directories(root);
    const auto directoryBytes = rgba8Dds({77U, 10U, 20U, 255U});
    writeBytes(root / "paint.dds", directoryBytes);
    auto value = conversion();
    addCandidate(value, 0U, 400, "DiffuseColor", "paint.dds", 1U);

    apex::render::FbxExternalTextureAuthorityResult result;
    {
        AssetSource source;
        source.addDirectory(root, "fbx");
        AcdArchive archive;
        archive.source = "fixture/data.acd";
        archive.assetName = "fixture";
        archive.entries.push_back(
            entry("paint.dds", rgba8Dds({1U, 2U, 3U, 255U})));
        source.addAcdArchive(std::move(archive));
        result = resolve_fbx_external_texture_authority(
            value, "fbx-root", source);
    }
    require(result.ok() && result.authority.resources.size() == 1U &&
                result.authority.resources.front().source_kind ==
                    apex::assets::AssetSourceKind::directory &&
                result.authority.resources.front().source_bytes == directoryBytes,
            "result owns directory-precedence bytes after source destruction");

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}

void rejectsChangedSourceWithoutPartialPublication() {
    const auto root = testRoot("changed");
    std::filesystem::create_directories(root);
    const auto bytes = rgba8Dds({1U, 2U, 3U, 255U});
    writeBytes(root / "paint.dds", bytes);
    AssetSource source;
    source.addDirectory(root, "fbx");
    writeBytes(root / "paint.dds", std::vector<std::uint8_t>{1U});

    auto value = conversion();
    addCandidate(value, 0U, 400, "DiffuseColor", "paint.dds", 1U);
    const auto result = resolve_fbx_external_texture_authority(
        value, "fbx-root", source);
    require(result.status == ExternalTextureAuthorityStatus::read_failed &&
                result.selections.empty() && result.authority.resources.empty(),
            "changed source fails without partial resource publication");

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}

} // namespace

int main() {
    try {
        selectsFirstExistingCandidateInNativeOrder();
        keepsFirstExistingSourceAndLeavesMissingNull();
        embeddedContentShadowsExternalLookup();
        rejectsAmbiguityBeforeLaterCandidates();
        rejectsMalformedCandidatesAndPayloads();
        enforcesCandidateAndMetadataLimits();
        retainsOwnedBytesAndDirectoryPrecedence();
        rejectsChangedSourceWithoutPartialPublication();
        std::cout << "FBX external texture authority tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FBX external texture authority tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
