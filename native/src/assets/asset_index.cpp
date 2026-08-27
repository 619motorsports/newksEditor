#include "apex/assets/asset_index.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <utility>

namespace apex::assets {
namespace {

[[noreturn]] void fail(std::string_view source, std::string_view code,
                       std::string_view message) {
    throw apex::core::ParseError("ASSET_INDEX", std::string(source), 0,
                                 std::string(code), std::string(message));
}

[[nodiscard]] std::string trim_ascii(std::string_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) ++first;
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) --last;
    return std::string(value.substr(first, last - first));
}

[[nodiscard]] std::string lower_ascii(std::string_view value) {
    // Deliberately ASCII-only.  Browser code uses Unicode case folding, but
    // asset paths are protocol-like identifiers and this keeps matching
    // deterministic across platforms (and avoids locale-dependent folding).
    std::string output(value);
    for (char& character : output) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
            character = static_cast<char>(byte + ('a' - 'A'));
    }
    return output;
}

[[nodiscard]] std::string path_for(const AssetFileInput& file) {
    if (!file.webkit_relative_path.empty()) return file.webkit_relative_path;
    if (!file.relative_path.empty()) return file.relative_path;
    return file.name;
}

[[nodiscard]] std::string relative_path(std::string_view path) {
    const auto slash = path.find('/');
    return slash == std::string_view::npos ? std::string(path)
                                           : std::string(path.substr(slash + 1));
}

[[nodiscard]] std::string basename(std::string_view path) {
    const auto slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

using Lookup = std::vector<std::pair<std::string, std::vector<std::size_t>>>;

void add_lookup(Lookup& lookup, std::string_view key, std::size_t index,
                const AssetIndexLimits& limits) {
    if (key.empty()) return;
    if (limits.maxMatches == 0)
        fail("asset index", "MATCH_LIMIT", "asset lookup matches exceed configured limit");
    const auto folded = lower_ascii(key);
    for (auto& item : lookup) {
        if (item.first == folded) {
            if (item.second.size() >= limits.maxMatches)
                fail("asset index", "MATCH_LIMIT", "asset lookup matches exceed configured limit");
            item.second.push_back(index);
            return;
        }
    }
    lookup.emplace_back(folded, std::vector<std::size_t>{index});
}

[[nodiscard]] const std::vector<std::size_t>* lookup(const Lookup& values,
                                                      std::string_view key) {
    const auto folded = lower_ascii(key);
    for (const auto& item : values) if (item.first == folded) return &item.second;
    return nullptr;
}

[[nodiscard]] bool natural_less(std::string_view left, std::string_view right) {
    // This is the bounded, deterministic substitute for the browser's
    // localeCompare({numeric:true}); it is intentionally ASCII-only.
    std::size_t a = 0, b = 0;
    while (a < left.size() && b < right.size()) {
        if (std::isdigit(static_cast<unsigned char>(left[a])) != 0 &&
            std::isdigit(static_cast<unsigned char>(right[b])) != 0) {
            std::size_t ae = a, be = b;
            while (ae < left.size() && std::isdigit(static_cast<unsigned char>(left[ae])) != 0) ++ae;
            while (be < right.size() && std::isdigit(static_cast<unsigned char>(right[be])) != 0) ++be;
            const auto an = left.substr(a, ae - a), bn = right.substr(b, be - b);
            const auto az = an.find_first_not_of('0'), bz = bn.find_first_not_of('0');
            const auto av = az == std::string_view::npos ? std::string_view("0") : an.substr(az);
            const auto bv = bz == std::string_view::npos ? std::string_view("0") : bn.substr(bz);
            if (av.size() != bv.size()) return av.size() < bv.size();
            if (av != bv) return av < bv;
            a = ae; b = be;
            continue;
        }
        const auto al = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(left[a])));
        const auto bl = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(right[b])));
        if (al != bl) return al < bl;
        ++a; ++b;
    }
    return left.size() < right.size();
}

[[nodiscard]] bool has_extension(std::string_view path, std::string_view extension) {
    const auto name = basename(path);
    if (name.size() < extension.size()) return false;
    return lower_ascii(name.substr(name.size() - extension.size())) == lower_ascii(extension);
}

}  // namespace

AssetPathResult normalize_asset_path_checked(std::string_view value, std::size_t max_bytes) {
    if (value.size() > max_bytes) return {false, {}, "path exceeds configured size limit"};
    auto source = trim_ascii(value);
    if (source.size() >= 2 && ((source.front() == '\'' && source.back() == '\'') ||
                               (source.front() == '"' && source.back() == '"'))) {
        source = source.substr(1, source.size() - 2);
    }
    while (!source.empty() && source.front() == '?') source.erase(source.begin());
    if (source.empty()) return {false, {}, "path is empty"};
    if (source.find('\0') != std::string::npos) return {false, {}, "path contains NUL"};
    std::replace(source.begin(), source.end(), '\\', '/');
    // Browser asset paths may carry a leading slash; remove it because this
    // index never resolves against the host filesystem.
    while (!source.empty() && source.front() == '/') source.erase(source.begin());
    if (source.empty()) return {false, {}, "path is empty"};
    if (source.size() >= 2 && std::isalpha(static_cast<unsigned char>(source[0])) != 0 && source[1] == ':')
        return {false, {}, "drive-qualified path is not allowed"};
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= source.size()) {
        const auto end = source.find('/', begin);
        const auto part = trim_ascii(source.substr(begin, end == std::string::npos
                                                              ? source.size() - begin
                                                              : end - begin));
        if (part == "..") {
            if (parts.empty()) return {false, {}, "parent traversal escapes asset root"};
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    std::string normalized;
    for (const auto& part : parts) {
        if (!normalized.empty()) normalized.push_back('/');
        normalized += part;
    }
    if (normalized.empty()) return {false, {}, "path is empty"};
    return {true, std::move(normalized), {}};
}

std::string normalize_asset_path(std::string_view value) {
    return normalize_asset_path_checked(value).path;
}

AssetFileIndex create_asset_file_index(std::span<const AssetFileInput> files,
                                       AssetIndexLimits limits) {
    if (files.size() > limits.maxFiles)
        fail("asset index", "FILE_LIMIT", "asset file count exceeds configured limit");
    AssetFileIndex result;
    result.entries.reserve(files.size());
    for (const auto& file : files) {
        if (file.name.size() > limits.maxPathBytes ||
            file.webkit_relative_path.size() > limits.maxPathBytes ||
            file.relative_path.size() > limits.maxPathBytes)
            continue;
        const auto normalized = normalize_asset_path_checked(path_for(file), limits.maxPathBytes);
        if (!normalized.accepted) continue;
        const auto relative = relative_path(normalized.path);
        const auto entry_index = result.entries.size();
        result.entries.push_back({file, normalized.path, relative, basename(normalized.path)});
        add_lookup(result.exact, normalized.path, entry_index, limits);
        if (lower_ascii(relative) != lower_ascii(normalized.path))
            add_lookup(result.exact, relative, entry_index, limits);
        add_lookup(result.basenames, basename(normalized.path), entry_index, limits);
    }
    return result;
}

AssetResolution resolve_asset_file(const AssetFileIndex& index, std::string_view requested,
                                   AssetIndexLimits limits) {
    if (index.entries.size() > limits.maxFiles)
        fail("asset resolution", "FILE_LIMIT", "asset index entry count exceeds configured limit");
    for (const auto& entry : index.entries) {
        if (entry.path.size() > limits.maxPathBytes || entry.relative_path.size() > limits.maxPathBytes ||
            entry.basename.size() > limits.maxPathBytes)
            fail("asset resolution", "PATH_LIMIT", "asset index path exceeds configured limit");
    }
    const auto normalized = normalize_asset_path_checked(requested, limits.maxPathBytes);
    AssetResolution result;
    result.requested_path = normalized.path;
    if (!normalized.accepted) {
        result.diagnostic = normalized.diagnostic;
        return result;
    }
    const auto key = lower_ascii(normalized.path);
    const auto finish = [&](const std::vector<std::size_t>& matches, AssetMatchKind kind) {
        if (matches.size() > limits.maxMatches)
            fail("asset resolution", "MATCH_LIMIT", "asset lookup matches exceed configured limit");
        for (const auto entry : matches) {
            if (entry >= index.entries.size())
                fail("asset resolution", "INVALID_INDEX", "asset lookup contains an invalid entry index");
        }
        result.matches = matches;
        result.matched_by = kind;
        if (matches.empty()) return;
        if (matches.size() == 1) {
            result.status = AssetResolutionStatus::resolved;
            result.file = matches.front();
            result.path = index.entries[result.file].path;
        } else result.status = AssetResolutionStatus::ambiguous;
    };
    if (const auto* exact = lookup(index.exact, key); exact != nullptr && !exact->empty()) {
        finish(*exact, AssetMatchKind::exact);
        return result;
    }
    std::vector<std::size_t> suffix;
    for (std::size_t index_value = 0; index_value < index.entries.size(); ++index_value) {
        const auto candidate = lower_ascii(index.entries[index_value].path);
        if (candidate.size() > key.size() && candidate.ends_with("/" + key)) {
            if (suffix.size() >= limits.maxMatches)
                fail("asset resolution", "MATCH_LIMIT", "asset lookup matches exceed configured limit");
            suffix.push_back(index_value);
        }
    }
    if (!suffix.empty()) {
        finish(suffix, AssetMatchKind::suffix);
        return result;
    }
    const auto slash = key.find_last_of('/');
    const auto base = slash == std::string::npos ? key : key.substr(slash + 1);
    if (const auto* named = lookup(index.basenames, base); named != nullptr && !named->empty()) {
        finish(*named, AssetMatchKind::basename);
        return result;
    }
    return result;
}

bool asset_folder_matches_model_files(const AssetFileIndex& index,
                                      std::span<const AssetModelDescriptor> descriptors,
                                      AssetIndexLimits limits) {
    if (descriptors.size() > limits.maxFiles)
        fail("model descriptors", "FILE_LIMIT", "model descriptor count exceeds configured limit");
    std::vector<const AssetFileInput*> models;
    for (const auto& descriptor : descriptors) {
        if (descriptor.file.name.size() > limits.maxPathBytes ||
            descriptor.file.webkit_relative_path.size() > limits.maxPathBytes ||
            descriptor.file.relative_path.size() > limits.maxPathBytes)
            fail("model descriptors", "PATH_LIMIT", "model descriptor path field exceeds configured limit");
        if (!descriptor.auxiliary) models.push_back(&descriptor.file);
    }
    if (models.empty()) return false;
    for (const auto* source : models) {
        const auto normalized = normalize_asset_path_checked(path_for(*source), limits.maxPathBytes);
        if (!normalized.accepted)
            fail("model descriptors", "UNSAFE_PATH", normalized.diagnostic);
        const auto& source_path = normalized.path;
        const auto source_relative = lower_ascii(relative_path(source_path));
        bool found = false;
        for (const auto& entry : index.entries) {
            const auto candidate_relative = lower_ascii(entry.relative_path);
            if (source_relative != candidate_relative || source->size != entry.file.size) continue;
            if (source->last_modified != 0 && entry.file.last_modified != 0 &&
                source->last_modified != entry.file.last_modified) continue;
            found = true;
            break;
        }
        if (!found) return false;
    }
    return true;
}

std::vector<std::string> external_resource_paths(
    std::span<const ExternalResourceReference> resources, AssetIndexLimits limits) {
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto& resource : resources) {
        const auto normalized = normalize_asset_path_checked(resource.file, limits.maxPathBytes);
        if (!normalized.accepted) continue;
        if (seen.insert(lower_ascii(normalized.path)).second) {
            if (result.size() >= limits.maxExternalPaths)
                fail("asset resources", "RESOURCE_LIMIT", "external resource output exceeds configured limit");
            result.push_back(normalized.path);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<AssetSkin> discover_asset_skins(const AssetFileIndex& index,
                                            AssetIndexLimits limits) {
    std::vector<AssetSkin> result;
    std::size_t total_skin_files = 0;
    for (const auto& entry : index.entries) {
        const auto relative = entry.relative_path;
        const auto first = relative.find('/');
        const auto second = first == std::string::npos ? std::string::npos : relative.find('/', first + 1);
        if (first == std::string::npos || second == std::string::npos ||
            lower_ascii(relative.substr(0, first)) != "skins" ||
            relative.find('/', second + 1) != std::string::npos) continue;
        const auto name = relative.substr(first + 1, second - first - 1);
        const auto filename = relative.substr(second + 1);
        if (name.empty() || (!has_extension(filename, ".dds") && !has_extension(filename, ".png") &&
                             !has_extension(filename, ".jpg") && !has_extension(filename, ".jpeg") &&
                             !has_extension(filename, ".webp") && lower_ascii(filename) != "ui_skin.json")) continue;
        const auto key = lower_ascii(name);
        auto found = std::find_if(result.begin(), result.end(), [&](const auto& skin) {
            return lower_ascii(skin.name) == key;
        });
        if (found == result.end()) {
            if (result.size() >= limits.maxSkins) fail("asset skins", "SKIN_LIMIT", "skin count exceeds configured limit");
            result.push_back({name, "skins/" + name, {}, {}});
            found = std::prev(result.end());
        }
        if (found->files.size() + found->metadata_files.size() >= limits.maxSkinFiles)
            fail("asset skins", "FILE_LIMIT", "skin file output exceeds configured limit");
        if (total_skin_files >= limits.maxSkinFiles)
            fail("asset skins", "FILE_LIMIT", "skin file output exceeds configured limit");
        ++total_skin_files;
        if (lower_ascii(filename) == "ui_skin.json") found->metadata_files.push_back(entry);
        else found->files.push_back(entry);
    }
    for (auto& skin : result) {
        std::sort(skin.files.begin(), skin.files.end(), [](const auto& left, const auto& right) {
            return natural_less(left.basename, right.basename);
        });
        std::sort(skin.metadata_files.begin(), skin.metadata_files.end(), [](const auto& left, const auto& right) {
            return natural_less(left.path, right.path);
        });
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return natural_less(left.name, right.name);
    });
    return result;
}

std::vector<AssetEntry> discover_asset_animations(const AssetFileIndex& index,
                                                  AssetIndexLimits limits) {
    std::vector<AssetEntry> result;
    for (const auto& entry : index.entries) {
        if (!has_extension(entry.relative_path, ".ksanim")) continue;
        if (result.size() >= limits.maxMatches) fail("asset animations", "MATCH_LIMIT", "animation output exceeds configured limit");
        result.push_back(entry);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return natural_less(left.relative_path, right.relative_path);
    });
    return result;
}

TextureMatchResult match_skin_textures(std::span<const AssetEntry> skin_files,
                                       std::span<const std::string> texture_names,
                                       AssetIndexLimits limits) {
    if (skin_files.size() > limits.maxTextureInputs ||
        texture_names.size() > limits.maxTextureInputs)
        fail("skin textures", "INPUT_LIMIT", "texture matching input count exceeds configured limit");
    // The first two checks above are intentionally explicit; this checked
    // sum avoids wrapping when both spans are large.
    if (skin_files.size() <= limits.maxTextureInputs &&
        texture_names.size() > limits.maxTextureInputs - skin_files.size())
        fail("skin textures", "INPUT_LIMIT", "texture matching input count exceeds configured limit");
    Lookup by_name;
    for (std::size_t index = 0; index < skin_files.size(); ++index) {
        if (skin_files[index].path.size() > limits.maxPathBytes ||
            skin_files[index].basename.size() > limits.maxPathBytes)
            fail("skin textures", "PATH_LIMIT", "skin texture path exceeds configured limit");
        add_lookup(by_name, basename(skin_files[index].basename.empty() ? skin_files[index].path : skin_files[index].basename), index, limits);
    }
    TextureMatchResult result;
    std::size_t output_count = 0;
    const auto reserve_output = [&](std::size_t count) {
        if (count > limits.maxTextureOutputs || output_count > limits.maxTextureOutputs - count)
            fail("skin textures", "OUTPUT_LIMIT", "texture matching output exceeds configured limit");
        output_count += count;
    };
    std::set<std::string> requested;
    for (const auto& raw : texture_names) {
        const auto normalized = normalize_asset_path_checked(raw, limits.maxPathBytes);
        if (!normalized.accepted) continue;
        const auto name = lower_ascii(basename(normalized.path));
        if (!requested.insert(name).second) continue;
        const auto* matches = lookup(by_name, name);
        if (matches == nullptr || matches->empty()) {
            reserve_output(1);
            result.missing.push_back(name);
        } else if (matches->size() == 1) {
            reserve_output(1);
            result.files.push_back({name, matches->front()});
        } else {
            if (matches->size() > limits.maxMatches)
                fail("skin textures", "MATCH_LIMIT", "texture lookup matches exceed configured limit");
            reserve_output(1 + matches->size());
            result.ambiguous.push_back({name, *matches});
        }
    }
    return result;
}

}  // namespace apex::assets
