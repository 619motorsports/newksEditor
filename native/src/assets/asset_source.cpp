#include "apex/assets/asset_source.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <cwchar>
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace apex::assets {

namespace {

using apex::core::ParseError;

[[nodiscard]] ParseError sourceError(std::string_view source, std::string_view code,
                                     std::string_view message) {
    return ParseError("ASSET_SOURCE", std::string(source), 0, std::string(code),
                      std::string(message));
}

[[nodiscard]] std::string lowerAscii(std::string_view value) {
    std::string output(value);
    for (auto& character : output) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
            character = static_cast<char>(byte + ('a' - 'A'));
    }
    return output;
}

[[nodiscard]] std::string trimAscii(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) --end;
    return std::string(value.substr(begin, end - begin));
}

struct NormalizedPath {
    std::string value;
    bool valid = false;
    std::string reason;
};

// The browser reference removes empty and dot components and strips a leading
// '?' used by URL-like resource names.  It also pops on '..'.  Native asset
// sources retain the useful normalization but reject traversal rather than
// allowing a path to escape its selected root.
[[nodiscard]] NormalizedPath normalizePath(std::string_view raw,
                                           std::size_t maxBytes,
                                           bool allowEmpty = false) {
    if (raw.size() > maxBytes) return {{}, false, "path exceeds configured size limit"};
    std::string source = trimAscii(raw);
    if (source.size() >= 2 &&
        ((source.front() == '\'' && source.back() == '\'') ||
         (source.front() == '"' && source.back() == '"'))) {
        source = source.substr(1, source.size() - 2);
    }
    while (!source.empty() && source.front() == '?') source.erase(source.begin());
    if (source.find('\0') != std::string::npos) return {{}, false, "path contains NUL"};
    std::replace(source.begin(), source.end(), '\\', '/');
    if (source.empty()) {
        if (allowEmpty) return {{}, true, {}};
        return {{}, false, "path is empty"};
    }
    if (source.front() == '/') return {{}, false, "absolute path is not allowed"};
    if (source.size() >= 2 && std::isalpha(static_cast<unsigned char>(source[0])) != 0 &&
        source[1] == ':')
        return {{}, false, "drive-qualified path is not allowed"};

    std::string normalized;
    std::size_t begin = 0;
    while (begin <= source.size()) {
        const auto end = source.find('/', begin);
        const auto rawPart = source.substr(begin, end == std::string::npos
                                                     ? source.size() - begin
                                                     : end - begin);
        const auto part = trimAscii(rawPart);
        if (part == "..") return {{}, false, "parent traversal is not allowed"};
        if (!part.empty() && part != ".") {
            if (!normalized.empty()) normalized.push_back('/');
            normalized += part;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (normalized.empty() && !allowEmpty) return {{}, false, "path is empty"};
    return {std::move(normalized), true, {}};
}

[[nodiscard]] bool isWithin(const std::filesystem::path& root,
                            const std::filesystem::path& candidate) {
    std::error_code error;
    const auto relative = std::filesystem::relative(candidate, root, error);
    if (error || relative.is_absolute()) return false;
    auto iterator = relative.begin();
    return iterator == relative.end() || *iterator != "..";
}

[[nodiscard]] std::string pathString(const std::filesystem::path& value) {
    return value.generic_string();
}

[[nodiscard]] std::string sourceLabel(const std::filesystem::path& root) {
    const auto value = root.generic_string();
    return value.empty() ? std::string("directory") : value;
}

[[nodiscard]] int sourcePriority(AssetSourceKind kind) {
    // An unpacked directory is the explicit authoring source and takes
    // precedence over the read-only packed fallback, matching app.js's
    // directory-first virtualAcdAssetEntry checks.
    return kind == AssetSourceKind::directory ? 0 : 1;
}

[[nodiscard]] bool hasFileCapacity(std::size_t current, std::size_t additional,
                                   std::size_t maximum) {
    return current <= maximum && additional <= maximum - current;
}

[[nodiscard]] std::string pathLimitMessage() {
    return "combined asset path exceeds configured size limit";
}

#if defined(__unix__) || defined(__APPLE__)

class ScopedFd final {
public:
    explicit ScopedFd(int descriptor = -1) : descriptor_(descriptor) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : descriptor_(other.descriptor_) { other.descriptor_ = -1; }
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            close();
            descriptor_ = other.descriptor_;
            other.descriptor_ = -1;
        }
        return *this;
    }
    ~ScopedFd() { close(); }

    [[nodiscard]] int get() const noexcept { return descriptor_; }
    [[nodiscard]] explicit operator bool() const noexcept { return descriptor_ >= 0; }

private:
    void close() noexcept {
        if (descriptor_ >= 0) {
            (void)::close(descriptor_);
            descriptor_ = -1;
        }
    }
    int descriptor_ = -1;
};

[[nodiscard]] std::vector<std::string> pathComponents(std::string_view value) {
    std::vector<std::string> components;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find('/', begin);
        components.emplace_back(value.substr(begin, end == std::string_view::npos
                                                       ? value.size() - begin
                                                       : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return components;
}

[[nodiscard]] std::vector<std::uint8_t> readConfinedPosix(
    const AssetFile& file, std::size_t expectedBytes, std::string_view source) {
#if !defined(O_DIRECTORY) || !defined(O_NOFOLLOW) || !defined(O_CLOEXEC)
    (void)file;
    (void)expectedBytes;
    throw sourceError(source, "UNSUPPORTED_SECURITY",
                      "confined directory reads are unavailable on this platform");
#else
    const ScopedFd root(::open(file.confinedRoot().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!root) throw sourceError(source, "UNSAFE_PATH", "cannot open confined asset root");
    ScopedFd current(::dup(root.get()));
    if (!current) throw sourceError(source, "READ_ERROR", "cannot duplicate confined asset root");
    const auto components = pathComponents(file.relativePath);
    if (components.empty()) throw sourceError(source, "UNSAFE_PATH", "empty confined asset path");
    for (std::size_t index = 0; index < components.size(); ++index) {
        const auto& component = components[index];
        if (component.empty() || component == "." || component == ".." || component.find('\0') != std::string::npos)
            throw sourceError(source, "UNSAFE_PATH", "unsafe confined asset path component");
        const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
                          (index + 1 < components.size() ? O_DIRECTORY : 0);
        const ScopedFd next(::openat(current.get(), component.c_str(), flags));
        if (!next) throw sourceError(source, errno == ELOOP ? "UNSAFE_PATH" : "READ_ERROR",
                                     errno == ELOOP ? "symlink encountered during confined read"
                                                    : "cannot open confined asset component");
        current = ScopedFd(::dup(next.get()));
        if (!current) throw sourceError(source, "READ_ERROR", "cannot retain confined asset handle");
    }
    struct stat status {};
    if (::fstat(current.get(), &status) != 0 || !S_ISREG(status.st_mode))
        throw sourceError(source, "UNSAFE_PATH", "confined asset is not a regular file");
    if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) != expectedBytes)
        throw sourceError(source, status.st_size >= 0 && static_cast<std::uintmax_t>(status.st_size) < expectedBytes
                                     ? "TRUNCATED"
                                     : "FILE_CHANGED",
                          "confined asset size changed");
    std::vector<std::uint8_t> data(expectedBytes);
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::read(current.get(), data.data() + offset, data.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw sourceError(source, "READ_ERROR", "confined asset read failed");
        }
        if (count == 0) throw sourceError(source, "TRUNCATED", "confined asset read was truncated");
        offset += static_cast<std::size_t>(count);
    }
    return data;
#endif
}

#endif

#if defined(_WIN32)

[[nodiscard]] std::vector<std::uint8_t> readConfinedWindows(
    const AssetFile& file, std::size_t expectedBytes, std::string_view source) {
    const auto handle = CreateFileW(file.filesystemPath.wstring().c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw sourceError(source, "UNSAFE_PATH", "cannot open directory asset handle");
    const auto closeHandle = [&] { (void)CloseHandle(handle); };
    const auto finalPathLength = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (finalPathLength == 0) {
        closeHandle();
        throw sourceError(source, "UNSUPPORTED_SECURITY", "cannot validate directory asset final path");
    }
    std::wstring finalPath(finalPathLength + 1, L'\0');
    const auto finalWritten = GetFinalPathNameByHandleW(handle, finalPath.data(),
                                                        static_cast<DWORD>(finalPath.size()),
                                                        FILE_NAME_NORMALIZED);
    if (finalWritten == 0) {
        closeHandle();
        throw sourceError(source, "UNSUPPORTED_SECURITY", "cannot validate directory asset final path");
    }
    const auto rootHandle = CreateFileW(file.confinedRoot().wstring().c_str(), FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (rootHandle == INVALID_HANDLE_VALUE) {
        closeHandle();
        throw sourceError(source, "UNSUPPORTED_SECURITY", "cannot validate asset root final path");
    }
    const auto rootLength = GetFinalPathNameByHandleW(rootHandle, nullptr, 0, FILE_NAME_NORMALIZED);
    std::wstring rootPath(rootLength + 1, L'\0');
    const auto rootWritten = rootLength == 0 ? 0 : GetFinalPathNameByHandleW(
        rootHandle, rootPath.data(), static_cast<DWORD>(rootPath.size()), FILE_NAME_NORMALIZED);
    (void)CloseHandle(rootHandle);
    if (rootLength == 0 || rootWritten == 0) {
        closeHandle();
        throw sourceError(source, "UNSUPPORTED_SECURITY", "cannot validate asset root final path");
    }
    if (finalWritten < finalPath.size()) finalPath.resize(finalWritten);
    if (rootWritten < rootPath.size()) rootPath.resize(rootWritten);
    while (rootPath.size() > 1 && (rootPath.back() == L'\\' || rootPath.back() == L'/'))
        rootPath.pop_back();
    if (finalPath.size() <= rootPath.size() ||
        _wcsnicmp(finalPath.c_str(), rootPath.c_str(), rootPath.size()) != 0 ||
        (finalPath.size() > rootPath.size() && finalPath[rootPath.size()] != L'\\')) {
        closeHandle();
        throw sourceError(source, "UNSAFE_PATH", "directory asset escaped source root");
    }
    LARGE_INTEGER size {};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0) {
        closeHandle();
        throw sourceError(source, "FILE_CHANGED", "directory asset size is unavailable");
    }
    if (static_cast<unsigned long long>(size.QuadPart) < expectedBytes) {
        closeHandle();
        throw sourceError(source, "TRUNCATED", "directory asset was truncated");
    }
    if (static_cast<unsigned long long>(size.QuadPart) != expectedBytes) {
        closeHandle();
        throw sourceError(source, "FILE_CHANGED", "directory asset size changed");
    }
    std::vector<std::uint8_t> data(expectedBytes);
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto request = static_cast<DWORD>(std::min<std::size_t>(data.size() - offset, 1u << 20u));
        DWORD readBytes = 0;
        if (!ReadFile(handle, data.data() + offset, request, &readBytes, nullptr)) {
            closeHandle();
            throw sourceError(source, "READ_ERROR", "directory asset read failed");
        }
        if (readBytes == 0) {
            closeHandle();
            throw sourceError(source, "TRUNCATED", "directory asset read was truncated");
        }
        offset += readBytes;
    }
    closeHandle();
    return data;
}

#endif

[[nodiscard]] bool samePointer(const AssetFile* left, const AssetFile* right) {
    return left == right;
}

} // namespace

AssetSource::AssetSource(AssetSourceLimits limits) : limits_(limits) {
    if (limits_.maxFiles == 0 || limits_.maxFileBytes > limits_.maxTotalBytes ||
        limits_.maxPathBytes == 0) {
        throw sourceError("asset-source", "INVALID_LIMIT", "asset source limits are inconsistent");
    }
}

void AssetSource::addDirectory(const std::filesystem::path& root, std::string_view rootLabel) {
    std::error_code error;
    const auto rootStatus = std::filesystem::symlink_status(root, error);
    if (error || !std::filesystem::is_directory(rootStatus))
        throw sourceError(pathString(root), "NOT_DIRECTORY", "asset source root is not a directory");
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error) throw sourceError(pathString(root), "PATH_ERROR", "cannot canonicalize asset source root");

    const auto label = normalizePath(rootLabel, limits_.maxPathBytes, true);
    if (!label.valid)
        throw sourceError(pathString(root), "UNSAFE_PATH", "invalid asset source label: " + label.reason);

    struct Candidate {
        std::filesystem::path path;
        std::filesystem::path canonical;
        std::uintmax_t bytes = 0;
    };
    std::vector<Candidate> candidates;
    std::filesystem::recursive_directory_iterator iterator(root, error), end;
    if (error) throw sourceError(pathString(root), "PATH_ERROR", "cannot enumerate asset source root");
    while (iterator != end) {
        const auto current = iterator->path();
        const auto status = std::filesystem::symlink_status(current, error);
        if (error) throw sourceError(pathString(current), "PATH_ERROR", "cannot inspect asset source path");
        if (std::filesystem::is_symlink(status)) {
            warnings_.push_back(sourceLabel(root) + ": ignored symlink asset path " + pathString(current));
            iterator.increment(error);
            if (error) throw sourceError(pathString(current), "PATH_ERROR", "cannot enumerate after symlink");
            continue;
        }
        if (std::filesystem::is_directory(status)) {
            iterator.increment(error);
            if (error) throw sourceError(pathString(current), "PATH_ERROR", "cannot enumerate asset directory");
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) {
            warnings_.push_back(sourceLabel(root) + ": ignored non-regular asset path " + pathString(current));
            iterator.increment(error);
            if (error) throw sourceError(pathString(current), "PATH_ERROR", "cannot enumerate asset path");
            continue;
        }
        const auto canonical = std::filesystem::weakly_canonical(current, error);
        if (error || !isWithin(canonicalRoot, canonical)) {
            warnings_.push_back(sourceLabel(root) + ": ignored asset path outside source root " + pathString(current));
            iterator.increment(error);
            if (error) throw sourceError(pathString(current), "PATH_ERROR", "cannot enumerate asset path");
            continue;
        }
        const auto bytes = std::filesystem::file_size(current, error);
        if (error) throw sourceError(pathString(current), "PATH_ERROR", "cannot inspect asset file size");
        if (bytes > limits_.maxFileBytes)
            throw sourceError(pathString(current), "FILE_TOO_LARGE", "asset file exceeds configured size limit");
        if (bytes > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
            throw sourceError(pathString(current), "FILE_TOO_LARGE", "asset file cannot be represented safely");
        const auto relative = std::filesystem::relative(canonical, canonicalRoot, error);
        if (error) throw sourceError(pathString(current), "PATH_ERROR", "cannot derive asset relative path");
        const auto safe = normalizePath(pathString(relative), limits_.maxPathBytes);
        if (!safe.valid)
            throw sourceError(pathString(current), "UNSAFE_PATH", "unsafe asset relative path: " + safe.reason);
        candidates.push_back({current, canonical, bytes});
        iterator.increment(error);
        if (error) throw sourceError(pathString(current), "PATH_ERROR", "cannot enumerate asset path");
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return pathString(left.canonical) < pathString(right.canonical);
    });
    if (!hasFileCapacity(files_.size(), candidates.size(), limits_.maxFiles))
        throw sourceError(pathString(root), "FILE_COUNT_LIMIT", "asset file count exceeds configured limit");
    const auto canonicalRootString = pathString(canonicalRoot);
    std::vector<AssetFile> pending;
    pending.reserve(candidates.size());
    std::size_t pendingBytes = 0;
    for (const auto& candidate : candidates) {
        const auto relativePath = normalizePath(
            pathString(std::filesystem::relative(candidate.canonical, canonicalRoot)),
            limits_.maxPathBytes);
        AssetFile file;
        file.kind = AssetSourceKind::directory;
        file.virtualFile = false;
        file.source = canonicalRootString;
        file.relativePath = relativePath.value;
        file.path = label.value.empty() ? relativePath.value : label.value + "/" + relativePath.value;
        if (file.path.size() > limits_.maxPathBytes)
            throw sourceError(pathString(candidate.path), "PATH_TOO_LONG", pathLimitMessage());
        file.filesystemPath = candidate.canonical;
        file.directoryRoot = canonicalRoot;
        file.size = static_cast<std::size_t>(candidate.bytes);
        file.lookupPaths = {file.path};
        if (file.relativePath != file.path) file.lookupPaths.push_back(file.relativePath);
        if (totalBytes_ > limits_.maxTotalBytes || pendingBytes > limits_.maxTotalBytes - totalBytes_ ||
            file.size > limits_.maxTotalBytes - totalBytes_ - pendingBytes)
            throw sourceError(pathString(candidate.path), "TOTAL_SIZE_LIMIT", "asset source bytes exceed configured limit");
        pendingBytes += file.size;
        pending.push_back(std::move(file));
    }
    for (const auto& file : pending) {
        for (const auto& alias : file.lookupPaths) {
            const auto lookup = lowerAscii(alias);
            for (const auto& existing : files_)
                for (const auto& existingAlias : existing.lookupPaths)
                    if (lowerAscii(existingAlias) == lookup)
                        warnings_.push_back(file.source + ": duplicate asset path " + alias);
            for (const auto& earlier : pending)
                if (&earlier < &file)
                    for (const auto& earlierAlias : earlier.lookupPaths)
                        if (lowerAscii(earlierAlias) == lookup)
                            warnings_.push_back(file.source + ": duplicate asset path " + alias);
        }
    }
    roots_.push_back(canonicalRoot);
    totalBytes_ += pendingBytes;
    for (auto& file : pending) files_.push_back(std::move(file));
}

void AssetSource::addAcdArchive(const apex::formats::AcdArchive& archive) {
    addAcdArchive(std::make_shared<const apex::formats::AcdArchive>(archive));
}

void AssetSource::addAcdArchive(std::shared_ptr<const apex::formats::AcdArchive> archive) {
    if (!archive) throw sourceError("data.acd", "INVALID_ARCHIVE", "null ACD archive");
    warnings_.insert(warnings_.end(), archive->warnings.begin(), archive->warnings.end());
    std::vector<AssetFile> pending;
    std::size_t pendingBytes = 0;
    for (std::size_t index = 0; index < archive->entries.size(); ++index) {
        const auto& entry = archive->entries[index];
        if (!entry.safe) {
            const auto reported = std::any_of(
                archive->warnings.begin(), archive->warnings.end(), [&](const auto& warning) {
                    return warning.find(entry.name) != std::string::npos;
                });
            if (!reported) warnings_.push_back(archive->source + ": ignored unsafe ACD entry " + entry.name);
            continue;
        }
        const auto safe = normalizePath(entry.path, limits_.maxPathBytes);
        if (!safe.valid) {
            warnings_.push_back(archive->source + ": ignored unsafe ACD entry " + entry.name);
            continue;
        }
        if (entry.data.size() > limits_.maxFileBytes)
            throw sourceError(archive->source, "FILE_TOO_LARGE", "ACD entry exceeds configured size limit");
        if (totalBytes_ > limits_.maxTotalBytes || pendingBytes > limits_.maxTotalBytes - totalBytes_ ||
            entry.data.size() > limits_.maxTotalBytes - totalBytes_ - pendingBytes)
            throw sourceError(archive->source, "TOTAL_SIZE_LIMIT", "asset source bytes exceed configured limit");
        AssetFile file;
        file.kind = AssetSourceKind::acd;
        file.virtualFile = true;
        file.source = archive->source;
        file.path = "data/" + safe.value;
        if (file.path.size() > limits_.maxPathBytes)
            throw sourceError(archive->source, "PATH_TOO_LONG", pathLimitMessage());
        file.relativePath = file.path;
        file.size = entry.data.size();
        file.lookupPaths = {file.path, safe.value};
        file.acdArchive = archive;
        file.acdEntryIndex = index;
        pendingBytes += file.size;
        pending.push_back(std::move(file));
    }
    if (!hasFileCapacity(files_.size(), pending.size(), limits_.maxFiles))
        throw sourceError(archive->source, "FILE_COUNT_LIMIT", "asset file count exceeds configured limit");
    for (const auto& file : pending) {
        for (const auto& alias : file.lookupPaths) {
            const auto lookup = lowerAscii(alias);
            for (const auto& existing : files_)
                for (const auto& existingAlias : existing.lookupPaths)
                    if (lowerAscii(existingAlias) == lookup)
                        warnings_.push_back(file.source + ": duplicate asset path " + alias);
            for (const auto& earlier : pending)
                if (&earlier < &file)
                    for (const auto& earlierAlias : earlier.lookupPaths)
                        if (lowerAscii(earlierAlias) == lookup)
                            warnings_.push_back(file.source + ": duplicate asset path " + alias);
        }
    }
    archives_.push_back(archive);
    totalBytes_ += pendingBytes;
    for (auto& file : pending) files_.push_back(std::move(file));
}

AssetResolution AssetSource::resolve(std::string_view requested) const {
    AssetResolution result;
    const auto normalized = normalizePath(requested, limits_.maxPathBytes);
    if (!normalized.valid) {
        result.status = AssetResolveStatus::rejected;
        result.diagnostic = normalized.reason;
        return result;
    }
    result.requestedPath = normalized.value;
    const auto key = lowerAscii(normalized.value);
    const auto findCandidates = [&](AssetMatchKind kind) {
        std::vector<const AssetFile*> matches;
        for (const auto& file : files_) {
            bool matched = false;
            if (kind == AssetMatchKind::exact) {
                for (const auto& alias : file.lookupPaths)
                    if (lowerAscii(alias) == key) {
                        matched = true;
                        break;
                    }
            } else {
                const auto candidate = lowerAscii(file.path);
                if (kind == AssetMatchKind::suffix) {
                    matched = candidate.size() > key.size() &&
                              candidate.compare(candidate.size() - key.size(), key.size(), key) == 0 &&
                              candidate[candidate.size() - key.size() - 1] == '/';
                } else {
                    const auto slash = key.find_last_of('/');
                    const auto basename = slash == std::string::npos ? key : key.substr(slash + 1);
                    const auto fileSlash = candidate.find_last_of('/');
                    const auto fileBasename = fileSlash == std::string::npos
                                                  ? candidate
                                                  : candidate.substr(fileSlash + 1);
                    matched = fileBasename == basename;
                }
            }
            if (matched && std::none_of(matches.begin(), matches.end(),
                                        [&](const auto* existing) { return samePointer(existing, &file); }))
                matches.push_back(&file);
        }
        return matches;
    };

    for (const auto kind : {AssetMatchKind::exact, AssetMatchKind::suffix, AssetMatchKind::basename}) {
        auto candidates = findCandidates(kind);
        if (candidates.empty()) continue;
        const auto bestPriority = std::min_element(
            candidates.begin(), candidates.end(), [](const auto* left, const auto* right) {
                return sourcePriority(left->kind) < sourcePriority(right->kind);
            });
        const auto priority = sourcePriority((*bestPriority)->kind);
        for (const auto* candidate : candidates)
            if (sourcePriority(candidate->kind) == priority) result.matches.push_back(candidate);
        result.matchedBy = kind;
        if (result.matches.size() == 1) {
            result.status = AssetResolveStatus::resolved;
            result.file = result.matches.front();
            if (candidates.size() > result.matches.size())
                result.diagnostic = "directory source takes precedence over packed ACD entry";
        } else {
            result.status = AssetResolveStatus::ambiguous;
            result.diagnostic = "multiple asset files match the requested path";
        }
        return result;
    }
    result.status = AssetResolveStatus::missing;
    return result;
}

std::vector<std::uint8_t> AssetSource::read(const AssetFile& file) const {
    const auto owned = std::find_if(files_.begin(), files_.end(),
                                    [&](const auto& candidate) { return &candidate == &file; });
    if (owned == files_.end()) throw sourceError("asset-source", "FOREIGN_FILE", "file is not owned by source");
    if (file.size > limits_.maxFileBytes)
        throw sourceError(file.source, "FILE_TOO_LARGE", "asset file exceeds configured size limit");
    if (file.kind == AssetSourceKind::acd) {
        if (!file.acdArchive || file.acdEntryIndex >= file.acdArchive->entries.size())
            throw sourceError(file.source, "INVALID_ENTRY", "ACD virtual entry is unavailable");
        const auto& entry = file.acdArchive->entries[file.acdEntryIndex];
        if (!entry.safe || entry.data.size() != file.size)
            throw sourceError(file.source, "FILE_CHANGED", "ACD virtual entry changed");
        return entry.data;
    }

#if defined(__unix__) || defined(__APPLE__)
    return readConfinedPosix(file, file.size, file.source);
#elif defined(_WIN32)
    return readConfinedWindows(file, file.size, file.source);
#else
    throw sourceError(file.source, "UNSUPPORTED_SECURITY",
                      "confined directory reads are unavailable on this platform");
#endif
}

} // namespace apex::assets
