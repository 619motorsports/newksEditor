#pragma once

#include "apex/core/parse_error.hpp"
#include "apex/formats/acd.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::assets {

// Asset paths come from INI files, archive records, and user-selected files.
// These limits are separate from format limits because a source can combine
// many individually valid files.
struct AssetSourceLimits {
    std::size_t maxFiles = 1'000'000;
    std::size_t maxFileBytes = 256u * 1024u * 1024u;
    std::size_t maxTotalBytes = 512u * 1024u * 1024u;
    std::size_t maxPathBytes = 1024u * 1024u;
    // Warnings are caller-visible metadata and are bounded independently of
    // the indexed file payloads. This prevents a noisy hostile directory or
    // archive from growing diagnostic storage without limit.
    std::size_t maxWarnings = 100'000;
    std::size_t maxWarningBytes = 16u * 1024u * 1024u;
};

enum class AssetSourceKind { directory, acd };

enum class AssetResolveStatus { resolved, ambiguous, missing, rejected };

enum class AssetMatchKind { none, exact, suffix, basename };

struct AssetFile {
    std::string path;
    std::string relativePath;
    std::string source;
    AssetSourceKind kind = AssetSourceKind::directory;
    bool virtualFile = false;
    std::size_t size = 0;

    // Directory-backed files retain their canonical path only for the source
    // owner. Call read() on AssetSource; do not open this path independently.
    std::filesystem::path filesystemPath;

    // All lookup aliases are normalized, source-relative names. They are
    // intentionally immutable through the public const file view. Case
    // folding is ASCII-only; this is a documented staged deviation from the
    // browser's full JavaScript String#toLowerCase behavior.
    std::vector<std::string> lookupPaths;

    [[nodiscard]] const std::filesystem::path& confinedRoot() const noexcept { return directoryRoot; }

private:
    friend class AssetSource;
    std::shared_ptr<const apex::formats::AcdArchive> acdArchive;
    std::size_t acdEntryIndex = 0;
    std::filesystem::path directoryRoot;
};

struct AssetResolution {
    AssetResolveStatus status = AssetResolveStatus::missing;
    AssetMatchKind matchedBy = AssetMatchKind::none;
    std::string requestedPath;
    std::string diagnostic;
    std::vector<const AssetFile*> matches;
    const AssetFile* file = nullptr;
};

class AssetSource final {
public:
    explicit AssetSource(AssetSourceLimits limits = {});

    // Index regular files below root. rootLabel preserves the browser index's
    // top-level path segment (for example car/data/lods.ini), while
    // relativePath remains data/lods.ini. Empty labels expose root-relative
    // paths in both fields.
    void addDirectory(const std::filesystem::path& root,
                      std::string_view rootLabel = {});

    // ACD data is copied into a shared immutable archive owner when passed by
    // reference, so AssetFile views remain valid after the caller returns.
    // Virtual ACD files are exposed under data/<entry> and also indexed by
    // <entry>, matching app.js's virtualAcdAssetEntry fallback.
    void addAcdArchive(const apex::formats::AcdArchive& archive);
    void addAcdArchive(std::shared_ptr<const apex::formats::AcdArchive> archive);

    [[nodiscard]] const AssetSourceLimits& limits() const noexcept { return limits_; }
    [[nodiscard]] std::span<const AssetFile> files() const noexcept { return files_; }
    [[nodiscard]] std::span<const std::string> warnings() const noexcept { return warnings_; }

    [[nodiscard]] AssetResolution resolve(std::string_view requested) const;

    // Read a resolved file with the same bounds and root-confinement checks as
    // indexing. POSIX uses root-relative openat/O_NOFOLLOW traversal and
    // Windows validates the final path obtained from a read handle. A
    // directory file that changes or truncates after indexing is rejected
    // rather than partially returned; unsupported platforms fail closed.
    [[nodiscard]] std::vector<std::uint8_t> read(const AssetFile& file) const;

private:
    AssetSourceLimits limits_;
    std::size_t totalBytes_ = 0;
    std::size_t warningBytes_ = 0;
    std::vector<AssetFile> files_;
    std::vector<std::string> warnings_;
    std::vector<std::shared_ptr<const apex::formats::AcdArchive>> archives_;
    std::vector<std::filesystem::path> roots_;
};

} // namespace apex::assets
