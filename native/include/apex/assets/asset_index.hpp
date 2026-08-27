#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex::assets {

struct AssetIndexLimits {
    std::size_t maxFiles = 1'000'000;
    std::size_t maxPathBytes = 1U * 1024U * 1024U;
    std::size_t maxMatches = 1'000'000;
    std::size_t maxSkins = 100'000;
    std::size_t maxSkinFiles = 1'000'000;
    std::size_t maxExternalPaths = 1'000'000;
    // Texture matching has separate aggregate guards because one request can
    // otherwise expand into many lookup and result records.
    std::size_t maxTextureInputs = 1'000'000;
    std::size_t maxTextureOutputs = 1'000'000;
};

struct AssetPathResult {
    bool accepted = false;
    std::string path;
    std::string diagnostic;
};

// Normalization rejects host-qualified/traversal paths as a security
// hardening boundary. This is stricter than the browser helper and never
// performs filesystem access.
[[nodiscard]] AssetPathResult normalize_asset_path_checked(
    std::string_view value, std::size_t max_bytes = 1U * 1024U * 1024U);
[[nodiscard]] std::string normalize_asset_path(std::string_view value);

struct AssetFileInput {
    std::string name;
    std::string webkit_relative_path;
    std::string relative_path;
    std::uint64_t size = 0;
    std::uint64_t last_modified = 0;
};

struct AssetEntry {
    AssetFileInput file;
    std::string path;
    std::string relative_path;
    std::string basename;
};

struct AssetFileIndex {
    std::vector<AssetEntry> entries;
    // Keys are ASCII case-folded normalized paths. Values are entry indices.
    std::vector<std::pair<std::string, std::vector<std::size_t>>> exact;
    std::vector<std::pair<std::string, std::vector<std::size_t>>> basenames;
};

[[nodiscard]] AssetFileIndex create_asset_file_index(
    std::span<const AssetFileInput> files, AssetIndexLimits limits = {});

enum class AssetResolutionStatus { resolved, ambiguous, missing };
enum class AssetMatchKind { none, exact, suffix, basename };

struct AssetResolution {
    AssetResolutionStatus status = AssetResolutionStatus::missing;
    AssetMatchKind matched_by = AssetMatchKind::none;
    std::string requested_path;
    std::vector<std::size_t> matches;
    std::size_t file = static_cast<std::size_t>(-1);
    std::string path;
    std::string diagnostic;
};

[[nodiscard]] AssetResolution resolve_asset_file(
    const AssetFileIndex& index, std::string_view requested,
    AssetIndexLimits limits = {});

struct AssetModelDescriptor {
    AssetFileInput file;
    bool auxiliary = false;
};

[[nodiscard]] bool asset_folder_matches_model_files(
    const AssetFileIndex& index, std::span<const AssetModelDescriptor> descriptors,
    AssetIndexLimits limits = {});

struct ExternalResourceReference {
    std::string file;
};

[[nodiscard]] std::vector<std::string> external_resource_paths(
    std::span<const ExternalResourceReference> resources,
    AssetIndexLimits limits = {});

struct AssetSkin {
    std::string name;
    std::string path;
    std::vector<AssetEntry> files;
    std::vector<AssetEntry> metadata_files;
};

[[nodiscard]] std::vector<AssetSkin> discover_asset_skins(
    const AssetFileIndex& index, AssetIndexLimits limits = {});
[[nodiscard]] std::vector<AssetEntry> discover_asset_animations(
    const AssetFileIndex& index, AssetIndexLimits limits = {});

struct TextureMatch {
    std::string name;
    std::size_t entry = static_cast<std::size_t>(-1);
};
struct TextureAmbiguous {
    std::string name;
    std::vector<std::size_t> entries;
};
struct TextureMatchResult {
    std::vector<TextureMatch> files;
    std::vector<TextureAmbiguous> ambiguous;
    std::vector<std::string> missing;
};

[[nodiscard]] TextureMatchResult match_skin_textures(
    std::span<const AssetEntry> skin_files,
    std::span<const std::string> texture_names, AssetIndexLimits limits = {});

}  // namespace apex::assets
