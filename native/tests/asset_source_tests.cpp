#include "apex/assets/asset_source.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::assets::AssetFile;
using apex::assets::AssetResolveStatus;
using apex::assets::AssetSource;
using apex::assets::AssetSourceLimits;
using apex::formats::AcdArchive;
using apex::formats::AcdEntry;
using apex::core::ParseError;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void writeFile(const std::filesystem::path& path, std::string_view value) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot create test file");
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}

AcdArchive archiveFixture() {
    AcdArchive archive;
    archive.source = "fixture/data.acd";
    archive.assetName = "example_car";
    AcdEntry lods;
    lods.name = "lods.ini";
    lods.path = "lods.ini";
    lods.safe = true;
    lods.data = {'p', 'a', 'c', 'k', 'e', 'd'};
    lods.size = lods.data.size();
    AcdEntry texture;
    texture.name = "textures/body.dds";
    texture.path = "textures/body.dds";
    texture.safe = true;
    texture.data = {1, 2, 3, 4};
    texture.size = texture.data.size();
    AcdEntry unsafe;
    unsafe.name = "../escape.ini";
    unsafe.path = "../escape.ini";
    unsafe.safe = false;
    unsafe.data = {'x'};
    unsafe.size = 1;
    archive.entries = {lods, texture, unsafe};
    return archive;
}

std::filesystem::path testRoot() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("apex-asset-source-" + std::to_string(static_cast<long long>(stamp)));
}

template <typename Function>
void expectsError(Function&& function, std::string_view code) {
    try {
        function();
    } catch (const ParseError& error) {
        require(error.format() == "ASSET_SOURCE", "source error attribution");
        require(error.code() == code, "unexpected asset-source error code");
        return;
    }
    throw std::runtime_error("asset-source operation accepted malformed input");
}

void resolvesDirectoryAndPackedSourcesWithPrecedence(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "data");
    std::filesystem::create_directories(root / "textures");
    std::filesystem::create_directories(root / "alternate");
    writeFile(root / "data/lods.ini", "directory");
    writeFile(root / "textures/body.dds", "external");
    // Keep the collision portable to case-insensitive filesystems: the
    // basename differs only by case, while the containing paths are distinct.
    writeFile(root / "alternate/BODY.DDS", "case collision");

    AssetSource source;
    source.addDirectory(root, "example_car");
    source.addAcdArchive(archiveFixture());

    const auto exact = source.resolve("data\\lods.ini");
    require(exact.status == AssetResolveStatus::resolved, "directory exact resolution");
    require(exact.file != nullptr && exact.file->kind == apex::assets::AssetSourceKind::directory,
            "directory takes precedence over packed ACD");
    require(exact.diagnostic.find("precedence") != std::string::npos, "precedence diagnostic");
    const auto bytes = source.read(*exact.file);
    require(std::string(bytes.begin(), bytes.end()) == "directory", "directory bytes");

    AssetSource packedSource;
    packedSource.addAcdArchive(archiveFixture());
    const auto packed = packedSource.resolve("lods.ini");
    require(packed.status == AssetResolveStatus::resolved && packed.file != nullptr,
            "packed suffix fallback");
    require(packed.file->kind == apex::assets::AssetSourceKind::acd, "packed fallback kind");
    require(packedSource.read(*packed.file).front() == 'p', "packed bytes");

    const auto ambiguous = source.resolve("body.dds");
    require(ambiguous.status == AssetResolveStatus::ambiguous, "basename collision remains ambiguous");
    require(ambiguous.matches.size() == 2, "basename collision candidates");
    require(!source.warnings().empty(), "duplicate diagnostics");
}

void rejectsUnsafeQueriesAndEnforcesLimits(const std::filesystem::path& root) {
    expectsError([&] {
        AssetSource invalid;
        invalid.addDirectory(root, "../escape");
    }, "UNSAFE_PATH");
    AssetSource source;
    source.addDirectory(root, "example_car");
    const std::array<std::string_view, 5> requests = {
        "../outside", "/absolute", "C:\\outside", "a/../b", std::string_view("bad\0name", 8)};
    for (const auto request : requests) {
        const auto result = source.resolve(request);
        require(result.status == AssetResolveStatus::rejected, "unsafe query rejected");
    }
    require(source.resolve("missing.ini").status == AssetResolveStatus::missing, "missing asset status");

    AssetSourceLimits fileLimit;
    fileLimit.maxFileBytes = 3;
    fileLimit.maxTotalBytes = 3;
    expectsError([&] {
        AssetSource limited(fileLimit);
        limited.addDirectory(root, "example_car");
    }, "FILE_TOO_LARGE");

    AssetSourceLimits countLimit;
    countLimit.maxFiles = 1;
    expectsError([&] {
        AssetSource limited(countLimit);
        limited.addDirectory(root, "example_car");
    }, "FILE_COUNT_LIMIT");

    const auto tinyRoot = root / "tiny-count";
    std::filesystem::create_directories(tinyRoot);
    writeFile(tinyRoot / "one.bin", "1");
    auto oneEntryArchive = archiveFixture();
    oneEntryArchive.entries.resize(1);
    AssetSourceLimits cumulativeLimit;
    cumulativeLimit.maxFiles = 2;
    AssetSource cumulative(cumulativeLimit);
    cumulative.addDirectory(tinyRoot, "tiny-count");
    cumulative.addAcdArchive(oneEntryArchive);
    expectsError([&] { cumulative.addAcdArchive(oneEntryArchive); }, "FILE_COUNT_LIMIT");

    const auto labelRoot = root / "tiny-label";
    std::filesystem::create_directories(labelRoot);
    writeFile(labelRoot / "x", "x");
    AssetSourceLimits pathLimit;
    pathLimit.maxPathBytes = 2;
    expectsError([&] {
        AssetSource bounded(pathLimit);
        bounded.addDirectory(labelRoot, "ab");
    }, "PATH_TOO_LONG");
}

void rejectsSymlinkEscapeAndReadTruncation(const std::filesystem::path& root,
                                           const std::filesystem::path& outside) {
    writeFile(outside, "secret");
    std::error_code symlinkError;
    std::filesystem::create_symlink(outside, root / "escape.txt", symlinkError);
    AssetSource source;
    source.addDirectory(root, "example_car");
    if (!symlinkError) {
        require(source.resolve("escape.txt").status == AssetResolveStatus::missing,
                "external symlink is not indexed");
        bool warned = false;
        for (const auto& warning : source.warnings())
            warned = warned || warning.find("symlink") != std::string::npos;
        require(warned, "symlink diagnostic");
    }

    const auto entry = source.resolve("data/lods.ini");
    require(entry.status == AssetResolveStatus::resolved && entry.file != nullptr, "truncation fixture entry");
    writeFile(root / "data/lods.ini", "x");
    expectsError([&] { (void)source.read(*entry.file); }, "TRUNCATED");
    if (!symlinkError) {
        std::error_code replaceError;
        std::filesystem::remove(root / "data/lods.ini", replaceError);
        std::filesystem::create_symlink(outside, root / "data/lods.ini", replaceError);
        if (!replaceError)
            expectsError([&] { (void)source.read(*entry.file); }, "UNSAFE_PATH");
        std::filesystem::remove(root / "data/lods.ini", replaceError);
        writeFile(root / "data/lods.ini", "directory");
    }
}

void rejectsForeignAndOversizedVirtualEntries(const std::filesystem::path& root) {
    AssetSource source;
    source.addDirectory(root, "example_car");
    const auto result = source.resolve("data/lods.ini");
    require(result.file != nullptr, "foreign-file fixture entry");
    AssetFile foreign = *result.file;
    expectsError([&] { (void)source.read(foreign); }, "FOREIGN_FILE");

    auto archive = archiveFixture();
    archive.entries.front().data.resize(7);
    archive.entries.front().size = 7;
    AssetSourceLimits limit;
    limit.maxFileBytes = 6;
    limit.maxTotalBytes = 6;
    expectsError([&] {
        AssetSource limited(limit);
        limited.addAcdArchive(archive);
    }, "FILE_TOO_LARGE");
}

} // namespace

int main() {
    const auto root = testRoot();
    const auto outside = root.parent_path() / (root.filename().string() + "-outside.txt");
    try {
        std::filesystem::create_directories(root);
        resolvesDirectoryAndPackedSourcesWithPrecedence(root);
        rejectsUnsafeQueriesAndEnforcesLimits(root);
        rejectsSymlinkEscapeAndReadTruncation(root, outside);
        rejectsForeignAndOversizedVirtualEntries(root);
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        std::filesystem::remove(outside, cleanupError);
        std::cout << "asset source tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        std::filesystem::remove(outside, cleanupError);
        std::cerr << "asset source tests failed: " << error.what() << '\n';
        return 1;
    }
}
