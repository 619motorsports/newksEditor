#pragma once

#include "apex/core/parse_error.hpp"
#include "apex/formats/ini.hpp"
#include "apex/formats/kn5.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex::workspace {

using WorkspaceError = apex::core::ParseError;
using Vector3 = std::array<float, 3>;

struct WorkspaceLimits {
    std::size_t maxManifestEntries = 100'000;
    std::size_t maxFiles = 100'000;
    std::size_t maxNodes = 10'000'000;
    std::size_t maxMaterials = 1'000'000;
    std::size_t maxTextures = 1'000'000;
    std::size_t maxTextureBytes = std::size_t{2} * 1024U * 1024U * 1024U;
    std::size_t maxAggregateBytes = std::size_t{4} * 1024U * 1024U * 1024U;
    std::size_t maxPathBytes = 1024U * 1024U;
};

struct TrackModelManifest {
    std::uint32_t index = 0;
    std::string file;
    Vector3 position{};
    Vector3 rotation{};
    std::string section;
    std::size_t line = 0;
};

struct DynamicObjectManifest {
    std::uint32_t index = 0;
    std::string file;
    float probability = 100.0F;
    std::array<float, 2> multiplicity = {1.0F, 1.0F};
    std::string posMode = "RANDOM";
    Vector3 positionCenter{};
    Vector3 positionRange{};
    std::string velMode = "RANDOM";
    Vector3 velocityBase{};
    Vector3 velocityRange{};
    std::string playWav;
    std::string section;
    std::size_t line = 0;
};

struct TrackManifest {
    std::string source;
    std::vector<TrackModelManifest> models;
    std::vector<DynamicObjectManifest> dynamicObjects;
    std::vector<std::string> warnings;
    std::size_t ignoredSections = 0;
};

struct CarLodManifest {
    std::uint32_t index = 0;
    std::string file;
    float inDistance = 0.0F;
    float outDistance = 0.0F;
    std::string section;
    std::size_t line = 0;
};

struct CarManifest {
    std::string source;
    std::vector<CarLodManifest> lods;
    std::optional<float> cockpitHrDistance;
    std::optional<float> driverHrDistance;
    std::vector<std::string> warnings;
    std::size_t ignoredSections = 0;
};

[[nodiscard]] TrackManifest parseModelsIni(
    const apex::formats::IniDocument& document,
    std::string_view source = "models.ini", WorkspaceLimits limits = {});
[[nodiscard]] TrackManifest parseModelsIni(
    std::string_view text, std::string source = "models.ini",
    apex::formats::IniParseLimits iniLimits = {}, WorkspaceLimits limits = {});

[[nodiscard]] CarManifest parseCarLodsIni(
    const apex::formats::IniDocument& document,
    std::string_view source = "data/lods.ini", WorkspaceLimits limits = {});
[[nodiscard]] CarManifest parseCarLodsIni(
    std::string_view text, std::string source = "data/lods.ini",
    apex::formats::IniParseLimits iniLimits = {}, WorkspaceLimits limits = {});

[[nodiscard]] std::vector<DynamicObjectManifest> contiguousDynamicTrackObjects(
    std::span<const DynamicObjectManifest> objects, std::vector<std::string>& warnings);

[[nodiscard]] apex::formats::Kn5Matrix4 modelPlacementMatrix(
    Vector3 position = {}, Vector3 rotation = {});

[[nodiscard]] bool carLodVisible(const CarLodManifest* lod, float distance,
                                 std::optional<std::uint32_t> selectedIndex = std::nullopt);
[[nodiscard]] float carLodDistance(float cameraDistance, float fovDegrees = 45.0F,
                                   float lodDistanceDivisor = 1.0F, bool trackCamera = false);

struct WorkspaceModelInput {
    std::string name;
    const apex::formats::Kn5File* model = nullptr;
    std::size_t size = 0;
    Vector3 position{};
    Vector3 rotation{};
    std::optional<std::uint32_t> manifestIndex;
    std::optional<CarLodManifest> lod;
    std::optional<DynamicObjectManifest> dynamic;
    std::string auxiliary;
};

struct WorkspaceOptions {
    std::string name = "KN5 workspace";
    std::string kind = "track";
    std::string manifest;
    std::vector<std::string> warnings;
    std::optional<float> cockpitHrDistance;
    std::optional<float> driverHrDistance;
    WorkspaceLimits limits{};
};

struct WorkspaceFile {
    std::string name;
    std::size_t size = 0;
    std::uint32_t version = 0;
    std::size_t materialOffset = 0;
    std::size_t materials = 0;
    std::size_t textures = 0;
    std::size_t textureBytes = 0;
    Vector3 position{};
    Vector3 rotation{};
    std::optional<CarLodManifest> lod;
    std::optional<std::uint32_t> manifestIndex;
    std::string auxiliary;
    std::optional<DynamicObjectManifest> dynamic;
    bool protectedFile = false;
};

struct WorkspaceTextureRecord {
    apex::formats::Kn5Texture texture;
    std::string workspaceFile;
    std::size_t workspaceFileIndex = 0;
};

struct WorkspaceMaterialRecord {
    apex::formats::Kn5Material material;
    std::string workspaceFile;
    std::size_t workspaceFileIndex = 0;
};

struct WorkspaceTextureCollision {
    std::string name;
    std::string previousFile;
    std::string replacementFile;
    bool sizesDiffer = false;
};

struct WorkspaceProtectedFile {
    std::string name;
    apex::formats::Kn5EncryptionInspection encryption;
};

struct WorkspaceMetadata {
    std::string name;
    std::string kind;
    std::string manifest;
    std::vector<WorkspaceFile> files;
    std::vector<std::uint32_t> versions;
    std::vector<WorkspaceTextureCollision> textureCollisions;
    std::vector<WorkspaceProtectedFile> protectedFiles;
    std::vector<WorkspaceTextureRecord> textureRecords;
    std::vector<WorkspaceMaterialRecord> materialRecords;
    std::vector<std::string> warnings;
    std::vector<std::string> deviations;
    std::optional<float> cockpitHrDistance;
    std::optional<float> driverHrDistance;
    bool scopeResources = false;
};

struct WorkspaceAssembly {
    apex::formats::Kn5File model;
    WorkspaceMetadata workspace;
};

[[nodiscard]] WorkspaceAssembly mergeKn5Models(
    std::span<const WorkspaceModelInput> entries, WorkspaceOptions options = {});

[[nodiscard]] WorkspaceAssembly assembleTrackWorkspace(
    const TrackManifest& manifest, std::span<const WorkspaceModelInput> available,
    WorkspaceOptions options = {});

[[nodiscard]] WorkspaceAssembly assembleCarLodWorkspace(
    const CarManifest& manifest, std::span<const WorkspaceModelInput> available,
    WorkspaceOptions options = {});

// Native callers that use snake-case format APIs can opt into the same names.
inline TrackManifest parse_models_ini(const apex::formats::IniDocument& document,
                                      std::string_view source = "models.ini",
                                      WorkspaceLimits limits = {}) {
    return parseModelsIni(document, source, limits);
}
inline CarManifest parse_car_lods_ini(const apex::formats::IniDocument& document,
                                      std::string_view source = "data/lods.ini",
                                      WorkspaceLimits limits = {}) {
    return parseCarLodsIni(document, source, limits);
}
inline WorkspaceAssembly merge_kn5_models(std::span<const WorkspaceModelInput> entries,
                                          WorkspaceOptions options = {}) {
    return mergeKn5Models(entries, std::move(options));
}

} // namespace apex::workspace
