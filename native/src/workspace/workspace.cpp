#include "apex/workspace/workspace.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <stdexcept>
#include <utility>

namespace apex::workspace {

namespace {

using apex::formats::IniDocument;
using apex::formats::IniSection;
using apex::formats::Kn5File;
using apex::formats::Kn5Material;
using apex::formats::Kn5Matrix4;
using apex::formats::Kn5Node;
using apex::formats::Kn5Texture;

constexpr float kPi = 3.14159265358979323846F;

[[nodiscard]] WorkspaceError error(std::string_view source, std::size_t line,
                                   std::string_view code, std::string_view message) {
    return WorkspaceError("WORKSPACE", std::string(source), line, std::string(code),
                          std::string(message));
}

[[nodiscard]] std::string trim(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) --end;
    return std::string(value.substr(begin, end - begin));
}

[[nodiscard]] std::string upperAscii(std::string_view value) {
    std::string output(value);
    for (auto& character : output) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z'))
            character = static_cast<char>(byte - ('a' - 'A'));
    }
    return output;
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

[[nodiscard]] std::string unquote(std::string_view value) {
    auto output = trim(value);
    if (output.size() >= 2 &&
        ((output.front() == '\'' && output.back() == '\'') ||
         (output.front() == '"' && output.back() == '"')))
        output = output.substr(1, output.size() - 2);
    return output;
}

void appendDocumentWarnings(const IniDocument& document, std::vector<std::string>& warnings) {
    for (const auto& warning : document.warnings)
        warnings.push_back(warning.source + ":" + std::to_string(warning.line) + ": " + warning.message);
}

[[nodiscard]] bool parseUnsignedIndex(std::string_view value, std::uint32_t& output) {
    if (value.empty()) return false;
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        parsed > std::numeric_limits<std::uint32_t>::max())
        return false;
    output = static_cast<std::uint32_t>(parsed);
    return true;
}

[[nodiscard]] bool parseFiniteFloat(std::string_view raw, float fallback, float& output) {
    const auto value = trim(raw);
    if (value.empty()) {
        output = fallback;
        return false;
    }
    char* end = nullptr;
    const auto parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0' || !std::isfinite(parsed) ||
        !std::isfinite(static_cast<float>(parsed))) {
        output = fallback;
        return false;
    }
    output = static_cast<float>(parsed);
    return true;
}

[[nodiscard]] float finiteNumber(const IniSection& section, std::string_view key, float fallback,
                                  std::vector<std::string>& warnings, std::string_view source,
                                  bool required = false) {
    const auto raw = section.last_value(key);
    if (raw.empty()) {
        if (required)
            warnings.push_back(std::string(source) + ":" + std::to_string(section.line) + ": " +
                               section.name + " has no " + std::string(key));
        return fallback;
    }
    float output = fallback;
    if (!parseFiniteFloat(raw, fallback, output))
        warnings.push_back(std::string(source) + ":" + std::to_string(section.line) + ": " +
                           section.name + " " + std::string(key) + " must be a finite number");
    return output;
}

[[nodiscard]] Vector3 vectorValue(const IniSection& section, std::string_view key,
                                   Vector3 fallback, std::vector<std::string>& warnings,
                                   std::string_view source) {
    const auto raw = section.last_value(key);
    if (raw.empty()) return fallback;
    const auto values = apex::formats::split_csp_list(raw);
    if (values.size() != 3)
        warnings.push_back(std::string(source) + ":" + std::to_string(section.line) + ": " +
                           section.name + " " + std::string(key) + " must contain three finite numbers");
    else {
        Vector3 output{};
        bool valid = true;
        for (std::size_t index = 0; index < output.size(); ++index)
            valid = parseFiniteFloat(values[index], fallback[index], output[index]) && valid;
        if (valid) return output;
        warnings.push_back(std::string(source) + ":" + std::to_string(section.line) + ": " +
                           section.name + " " + std::string(key) + " must contain three finite numbers");
    }
    return fallback;
}

[[nodiscard]] std::array<float, 2> multiplicityValue(
    const IniSection& section, std::vector<std::string>& warnings, std::string_view source) {
    const auto raw = section.last_value("MULT", "1,1");
    const auto values = apex::formats::split_csp_list(raw);
    std::array<float, 2> output = {1.0F, 1.0F};
    bool valid = values.size() == output.size();
    if (valid)
        for (std::size_t index = 0; index < output.size(); ++index)
            valid = parseFiniteFloat(values[index], output[index], output[index]) && valid;
    if (!valid)
        warnings.push_back(std::string(source) + ":" + std::to_string(section.line) + ": " +
                           section.name + " MULT must contain two finite numbers");
    return output;
}

[[nodiscard]] bool portableManifestFile(std::string_view raw, std::size_t maxBytes,
                                         std::string& normalized, std::string& reason) {
    normalized = unquote(raw);
    if (normalized.empty()) {
        reason = "manifest entry has no file name";
        return false;
    }
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.size() > maxBytes || normalized.front() == '/' || normalized.find('\0') != std::string::npos ||
        (normalized.size() >= 2 && std::isalpha(static_cast<unsigned char>(normalized[0])) != 0 &&
         normalized[1] == ':')) {
        reason = "manifest file name must be relative and bounded";
        return false;
    }
    std::size_t begin = 0;
    while (begin <= normalized.size()) {
        const auto end = normalized.find('/', begin);
        const auto part = normalized.substr(begin, end == std::string::npos
                                                       ? normalized.size() - begin
                                                       : end - begin);
        if (part.empty() || part == ".") {
            reason = "manifest file name contains an empty or dot path component";
            return false;
        }
        if (part == "..") {
            reason = "manifest file name contains parent traversal";
            return false;
        }
        const auto deviceName = upperAscii(part.substr(0, part.find('.')));
        if (part.back() == '.' || part.back() == ' ' ||
            part.find_first_of("<>:\"|?*") != std::string::npos ||
            deviceName == "CON" || deviceName == "PRN" || deviceName == "AUX" ||
            deviceName == "NUL" ||
            ((deviceName.size() == 4 && (deviceName.rfind("COM", 0) == 0 ||
                                         deviceName.rfind("LPT", 0) == 0)) &&
             deviceName.back() >= '1' && deviceName.back() <= '9')) {
            reason = "manifest file name is not portable";
            return false;
        }
        for (const auto character : part)
            if (static_cast<unsigned char>(character) < 0x20U || character == '\x7f') {
                reason = "manifest file name contains a control character";
                return false;
            }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

[[nodiscard]] std::string sectionSuffix(std::string_view name, std::string_view prefix,
                                        std::uint32_t& index) {
    const auto upper = upperAscii(name);
    if (upper.rfind(prefix, 0) != 0) return {};
    const auto suffix = upper.substr(prefix.size());
    if (!parseUnsignedIndex(suffix, index)) return {};
    return upper;
}

[[nodiscard]] std::string normalizedInputName(std::string_view value, std::size_t maxPathBytes) {
    std::string output;
    std::string reason;
    if (!portableManifestFile(value, maxPathBytes, output, reason))
        throw error("workspace", 0, "UNSAFE_REFERENCE", reason);
    return output;
}

[[nodiscard]] std::size_t checkedAdd(std::size_t left, std::size_t right,
                                     std::string_view source, std::string_view what) {
    if (right > std::numeric_limits<std::size_t>::max() - left)
        throw error(source, 0, "SIZE_OVERFLOW", std::string(what) + " size overflows");
    return left + right;
}

[[nodiscard]] Kn5Matrix4 multiply(const Kn5Matrix4& left, const Kn5Matrix4& right) {
    Kn5Matrix4 output{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4; ++column)
            for (std::size_t index = 0; index < 4; ++index)
                output[row * 4 + column] += left[row * 4 + index] * right[index * 4 + column];
    return output;
}

[[nodiscard]] Kn5Node remapNode(const Kn5Node& source, std::size_t materialOffset,
                                std::size_t materialCount, std::size_t& nodeCount,
                                const WorkspaceLimits& limits, std::string_view file) {
    if (nodeCount >= limits.maxNodes)
        throw error(file, 0, "NODE_LIMIT", "workspace node count exceeds configured limit");
    ++nodeCount;
    Kn5Node output = source;
    if ((source.kind == "mesh" || source.kind == "skinnedMesh") &&
        static_cast<std::size_t>(source.materialId) >= materialCount)
        throw error(file, 0, "INVALID_REFERENCE", "mesh material ID exceeds model material count");
    if (source.kind == "mesh" || source.kind == "skinnedMesh") {
        if (static_cast<std::size_t>(source.materialId) >
            std::numeric_limits<std::uint32_t>::max() - materialOffset)
            throw error(file, 0, "SIZE_OVERFLOW", "workspace material ID overflows");
        output.materialId = source.materialId + static_cast<std::uint32_t>(materialOffset);
    }
    output.children.clear();
    output.children.reserve(source.children.size());
    for (const auto& child : source.children)
        output.children.push_back(remapNode(child, materialOffset, materialCount, nodeCount, limits, file));
    return output;
}

[[nodiscard]] std::size_t nodeCount(const Kn5Node& node) {
    std::size_t count = 1;
    for (const auto& child : node.children) count = checkedAdd(count, nodeCount(child), "workspace", "node");
    return count;
}

[[nodiscard]] std::string textureKey(std::string_view value) { return lowerAscii(value); }

[[nodiscard]] const WorkspaceModelInput* matchAvailable(
    std::string_view file, std::span<const WorkspaceModelInput> available,
    std::vector<bool>& referenced, std::string_view source) {
    const auto wanted = lowerAscii(file);
    const WorkspaceModelInput* match = nullptr;
    for (std::size_t index = 0; index < available.size(); ++index) {
        if (lowerAscii(available[index].name) != wanted) continue;
        if (match != nullptr)
            throw error(source, 0, "AMBIGUOUS_REFERENCE", "manifest file resolves to duplicate model inputs");
        match = &available[index];
        referenced[index] = true;
    }
    if (match == nullptr) throw error(source, 0, "INVALID_REFERENCE", "manifest file has no supplied KN5 model");
    return match;
}

} // namespace

TrackManifest parseModelsIni(const IniDocument& document, std::string_view source,
                             WorkspaceLimits limits) {
    TrackManifest output;
    output.source = std::string(source);
    appendDocumentWarnings(document, output.warnings);
    std::size_t recognized = 0;
    std::size_t manifestSections = 0;
    for (const auto& section : document.sections) {
        std::uint32_t index = 0;
        const auto modelName = sectionSuffix(section.name, "MODEL_", index);
        const auto dynamicName = sectionSuffix(section.name, "DYNAMIC_OBJECT_", index);
        if (modelName.empty() && dynamicName.empty()) {
            const auto upper = upperAscii(section.name);
            if (upper.rfind("MODEL_", 0) == 0 || upper.rfind("DYNAMIC_OBJECT_", 0) == 0)
                output.warnings.push_back(std::string(source) + ":" + std::to_string(section.line) +
                                          ": " + section.name + " has an invalid numeric index");
            continue;
        }
        if (manifestSections >= limits.maxManifestEntries)
            throw error(source, section.line, "COUNT_LIMIT", "manifest entry count exceeds configured limit");
        ++manifestSections;
        auto file = unquote(section.last_value("FILE"));
        if (file.empty()) {
            output.warnings.push_back(std::string(source) + ":" + std::to_string(section.line) + ": " +
                                      section.name + " has no FILE");
            continue;
        }
        std::string normalized;
        std::string reason;
        if (!portableManifestFile(file, limits.maxPathBytes, normalized, reason)) {
            output.warnings.push_back(std::string(source) + ":" + std::to_string(section.line) + ": " + reason);
            continue;
        }
        ++recognized;
        if (!modelName.empty()) {
            output.models.push_back({index, normalized,
                                     vectorValue(section, "POSITION", {}, output.warnings, source),
                                     vectorValue(section, "ROTATION", {}, output.warnings, source),
                                     section.name, section.line});
            continue;
        }
        auto probability = finiteNumber(section, "PROBABILITY", 100.0F, output.warnings, source);
        if (probability < 0.0F || probability > 100.0F)
            output.warnings.push_back(std::string(source) + ":" + std::to_string(section.line) + ": " +
                                      section.name + " PROBABILITY should be from 0 to 100");
        DynamicObjectManifest dynamic;
        dynamic.index = index;
        dynamic.file = normalized;
        dynamic.probability = probability;
        dynamic.multiplicity = multiplicityValue(section, output.warnings, source);
        dynamic.posMode = upperAscii(trim(section.last_value("POS_MODE", "RANDOM")));
        dynamic.positionCenter = vectorValue(section, "RND_POS_CENTER", {}, output.warnings, source);
        dynamic.positionRange = vectorValue(section, "RND_POS_RANGE", {}, output.warnings, source);
        dynamic.velMode = upperAscii(trim(section.last_value("VEL_MODE", "RANDOM")));
        dynamic.velocityBase = vectorValue(section, "RND_VEL_BASE", {}, output.warnings, source);
        dynamic.velocityRange = vectorValue(section, "RND_VEL_RANGE", {}, output.warnings, source);
        dynamic.playWav = unquote(section.last_value("PLAY_WAV"));
        dynamic.section = section.name;
        dynamic.line = section.line;
        output.dynamicObjects.push_back(std::move(dynamic));
    }
    std::stable_sort(output.models.begin(), output.models.end(),
                     [](const auto& left, const auto& right) { return left.index < right.index; });
    std::stable_sort(output.dynamicObjects.begin(), output.dynamicObjects.end(),
                     [](const auto& left, const auto& right) { return left.index < right.index; });
    output.ignoredSections = document.sections.size() >= recognized
                                 ? document.sections.size() - recognized
                                 : 0;
    return output;
}

TrackManifest parseModelsIni(std::string_view text, std::string source,
                             apex::formats::IniParseLimits iniLimits, WorkspaceLimits limits) {
    return parseModelsIni(apex::formats::parse_csp_ini(text, source, iniLimits), source, limits);
}

CarManifest parseCarLodsIni(const IniDocument& document, std::string_view source,
                            WorkspaceLimits limits) {
    CarManifest output;
    output.source = std::string(source);
    appendDocumentWarnings(document, output.warnings);
    std::map<std::uint32_t, const IniSection*> sections;
    std::size_t lodSections = 0;
    for (const auto& section : document.sections) {
        std::uint32_t index = 0;
        if (sectionSuffix(section.name, "LOD_", index).empty()) {
            if (upperAscii(section.name).rfind("LOD_", 0) == 0)
                output.warnings.push_back(std::string(source) + ":" + std::to_string(section.line) +
                                          ": " + section.name + " has an invalid numeric index");
            continue;
        }
        if (lodSections >= limits.maxManifestEntries)
            throw error(source, section.line, "COUNT_LIMIT", "LOD section count exceeds configured limit");
        ++lodSections;
        if (sections.contains(index))
            output.warnings.push_back(std::string(source) + ":" + std::to_string(section.line) +
                                      ": duplicate LOD_" + std::to_string(index) +
                                      " section replaces the earlier section");
        sections[index] = &section;
    }
    std::uint64_t contiguousIndex = 0;
    while (contiguousIndex <= std::numeric_limits<std::uint32_t>::max() &&
           sections.contains(static_cast<std::uint32_t>(contiguousIndex))) {
        const auto index = static_cast<std::uint32_t>(contiguousIndex);
        const auto& section = *sections[index];
        auto file = unquote(section.last_value("FILE"));
        if (file.empty()) {
            output.warnings.push_back(std::string(source) + ":" + std::to_string(section.line) + ": " +
                                      section.name + " has no FILE");
            file.clear();
        } else {
            std::string normalized;
            std::string reason;
            if (!portableManifestFile(file, limits.maxPathBytes, normalized, reason)) {
                output.warnings.push_back(std::string(source) + ":" + std::to_string(section.line) + ": " + reason);
                file.clear();
            } else {
                file = std::move(normalized);
            }
        }
        const auto lodIn = finiteNumber(section, "IN", 0.0F, output.warnings, source, true);
        const auto lodOut = finiteNumber(section, "OUT", 0.0F, output.warnings, source, true);
        if (lodIn < 0.0F || lodOut < 0.0F)
            output.warnings.push_back(std::string(source) + ":" + std::to_string(section.line) + ": " +
                                      section.name + " has a negative distance");
        if (lodOut <= lodIn)
            output.warnings.push_back(std::string(source) + ":" + std::to_string(section.line) + ": " +
                                      section.name + " OUT must be greater than IN");
        output.lods.push_back({index, file, lodIn, lodOut, section.name, section.line});
        ++contiguousIndex;
    }
    if (output.lods.empty())
        output.warnings.push_back(std::string(source) + ": no contiguous LOD_0 section was found");
    for (const auto& [index, section] : sections)
        if (index >= output.lods.size())
            output.warnings.push_back(std::string(source) + ": LOD_" + std::to_string(index) +
                                      " is ignored because the game stops at missing LOD_" +
                                      std::to_string(output.lods.size()));
    for (std::size_t index = 1; index < output.lods.size(); ++index) {
        const auto& previous = output.lods[index - 1];
        const auto& current = output.lods[index];
        if (current.inDistance > previous.outDistance)
            output.warnings.push_back(std::string(source) + ":" + std::to_string(current.line) + ": " +
                                      previous.section + " and " + current.section + " leave a " +
                                      std::to_string(current.inDistance - previous.outDistance) + " m gap");
        else if (current.inDistance < previous.outDistance)
            output.warnings.push_back(std::string(source) + ":" + std::to_string(current.line) + ": " +
                                      previous.section + " and " + current.section + " overlap by " +
                                      std::to_string(previous.outDistance - current.inDistance) + " m");
    }
    const auto switchDistance = [&](std::string_view name) -> std::optional<float> {
        const auto wanted = upperAscii(name);
        for (const auto& section : document.sections)
            if (upperAscii(section.name) == wanted && section.last_entry("DISTANCE_SWITCH") != nullptr)
                return finiteNumber(section, "DISTANCE_SWITCH", 0.0F, output.warnings, source);
        return std::nullopt;
    };
    output.cockpitHrDistance = switchDistance("COCKPIT_HR");
    output.driverHrDistance = switchDistance("DRIVER_HR");
    output.ignoredSections = document.sections.size() >= sections.size()
                                 ? document.sections.size() - sections.size()
                                 : 0;
    return output;
}

CarManifest parseCarLodsIni(std::string_view text, std::string source,
                            apex::formats::IniParseLimits iniLimits, WorkspaceLimits limits) {
    return parseCarLodsIni(apex::formats::parse_csp_ini(text, source, iniLimits), source, limits);
}

std::vector<DynamicObjectManifest> contiguousDynamicTrackObjects(
    std::span<const DynamicObjectManifest> objects, std::vector<std::string>& warnings) {
    std::map<std::uint32_t, const DynamicObjectManifest*> byIndex;
    for (const auto& object : objects) {
        if (byIndex.contains(object.index)) {
            warnings.push_back("DYNAMIC_OBJECT_" + std::to_string(object.index) +
                               " is duplicated; the preview uses its first entry");
            continue;
        }
        byIndex.emplace(object.index, &object);
    }
    std::vector<DynamicObjectManifest> output;
    for (std::uint64_t index = 0;
         index <= std::numeric_limits<std::uint32_t>::max() &&
         byIndex.contains(static_cast<std::uint32_t>(index)); ++index)
        output.push_back(*byIndex[static_cast<std::uint32_t>(index)]);
    if (output.size() < byIndex.size())
        warnings.push_back("Dynamic objects stop at missing DYNAMIC_OBJECT_" +
                           std::to_string(output.size()) + "; the preview ignores " +
                           std::to_string(byIndex.size() - output.size()) + " later section" +
                           (byIndex.size() - output.size() == 1 ? "" : "s"));
    return output;
}

Kn5Matrix4 modelPlacementMatrix(Vector3 position, Vector3 rotation) {
    const auto finiteVector = [](const Vector3& value) {
        return std::all_of(value.begin(), value.end(), [](float component) {
            return std::isfinite(component);
        });
    };
    if (!finiteVector(position) || !finiteVector(rotation))
        throw std::invalid_argument("workspace placement position and rotation must be finite");
    const auto radians = [&](float value) { return static_cast<double>(value) * static_cast<double>(kPi) / 180.0; };
    const auto heading = radians(rotation[0]);
    const auto pitch = radians(rotation[1]);
    const auto roll = radians(rotation[2]);
    const auto cy = std::cos(heading), sy = std::sin(heading);
    const auto cx = std::cos(pitch), sx = std::sin(pitch);
    const auto cz = std::cos(roll), sz = std::sin(roll);
    const Kn5Matrix4 yaw = {static_cast<float>(cy), 0, static_cast<float>(sy), 0,
                            0, 1, 0, 0, static_cast<float>(-sy), 0, static_cast<float>(cy), 0,
                            0, 0, 0, 1};
    const Kn5Matrix4 pitchMatrix = {1, 0, 0, 0, 0, static_cast<float>(cx), static_cast<float>(-sx), 0,
                                    0, static_cast<float>(sx), static_cast<float>(cx), 0, 0, 0, 0, 1};
    const Kn5Matrix4 rollMatrix = {static_cast<float>(cz), static_cast<float>(-sz), 0, 0,
                                   static_cast<float>(sz), static_cast<float>(cz), 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    auto output = multiply(multiply(yaw, pitchMatrix), rollMatrix);
    output[12] = position[0];
    output[13] = position[1];
    output[14] = position[2];
    return output;
}

bool carLodVisible(const CarLodManifest* lod, float distance,
                   std::optional<std::uint32_t> selectedIndex) {
    if (lod == nullptr) return true;
    if (selectedIndex.has_value()) return lod->index == *selectedIndex;
    const auto value = std::isnan(distance) ? 0.0F : std::max(0.0F, distance);
    return value >= lod->inDistance && value < lod->outDistance;
}

float carLodDistance(float cameraDistance, float fovDegrees, float lodDistanceDivisor,
                     bool trackCamera) {
    const auto distance = std::isnan(cameraDistance) ? 0.0F : std::max(0.0F, cameraDistance);
    const auto fov = std::isnan(fovDegrees) ? 0.0F : std::max(0.0F, fovDegrees);
    const auto divisorInput = std::isnan(lodDistanceDivisor) || lodDistanceDivisor == 0.0F
                                  ? 1.0F
                                  : lodDistanceDivisor;
    auto divisor = std::abs(divisorInput);
    divisor = std::max(std::numeric_limits<float>::epsilon(), divisor);
    if (trackCamera) divisor *= 10.0F;
    return distance * fov / 60.0F / divisor;
}

WorkspaceAssembly mergeKn5Models(std::span<const WorkspaceModelInput> entries,
                                 WorkspaceOptions options) {
    if (entries.empty()) throw error(options.manifest, 0, "EMPTY_WORKSPACE", "a KN5 workspace needs at least one model");
    if (entries.size() > options.limits.maxFiles)
        throw error(options.manifest, 0, "COUNT_LIMIT", "workspace file count exceeds configured limit");
    WorkspaceAssembly output;
    output.model.magic = "sc6969";
    output.model.source = options.manifest.empty() ? "workspace" : options.manifest;
    output.model.root.kind = "node";
    output.model.root.type = 1;
    output.model.root.name = options.name;
    output.model.root.active = true;
    output.model.root.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    output.workspace.name = options.name;
    output.workspace.kind = options.kind;
    output.workspace.manifest = options.manifest;
    output.workspace.warnings = std::move(options.warnings);
    output.workspace.cockpitHrDistance = options.cockpitHrDistance;
    output.workspace.driverHrDistance = options.driverHrDistance;
    output.workspace.scopeResources = options.kind == "carLods" ||
                                     std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
                                         return entry.auxiliary == "reflectionEnvironment";
                                     });
    output.workspace.deviations.push_back("Texture case folding uses ASCII-only lower-casing during this staged port");
    std::map<std::string, std::size_t> textureMap;
    std::set<std::uint32_t> versions;
    std::size_t nodeCountValue = 1;
    std::size_t aggregateBytes = 0;
    std::size_t textureBytes = 0;

    for (std::size_t fileIndex = 0; fileIndex < entries.size(); ++fileIndex) {
        const auto& entry = entries[fileIndex];
        if (entry.model == nullptr)
            throw error(entry.name, 0, "INVALID_MODEL", "workspace entry is not a parsed KN5 model");
        const auto& model = *entry.model;
        const auto name = normalizedInputName(entry.name, options.limits.maxPathBytes);
        if (!std::all_of(entry.position.begin(), entry.position.end(),
                         [](float component) { return std::isfinite(component); }) ||
            !std::all_of(entry.rotation.begin(), entry.rotation.end(),
                         [](float component) { return std::isfinite(component); }))
            throw error(name, 0, "NON_FINITE_TRANSFORM",
                        "workspace placement position and rotation must be finite");
        if (model.materials.size() > options.limits.maxMaterials -
                                       std::min(output.model.materials.size(), options.limits.maxMaterials))
            throw error(name, 0, "MATERIAL_LIMIT", "workspace material count exceeds configured limit");
        if (model.textures.size() > options.limits.maxTextures)
            throw error(name, 0, "TEXTURE_LIMIT", "workspace texture count exceeds configured limit");
        const auto modelBytes = entry.size == 0 ? model.byteLength : entry.size;
        if (modelBytes > options.limits.maxAggregateBytes ||
            aggregateBytes > options.limits.maxAggregateBytes - modelBytes)
            throw error(name, 0, "SIZE_LIMIT", "workspace aggregate bytes exceed configured limit");
        aggregateBytes = checkedAdd(aggregateBytes, modelBytes, name, "workspace aggregate");
        const auto modelNodes = nodeCount(model.root);
        if (nodeCountValue > options.limits.maxNodes || modelNodes > options.limits.maxNodes - nodeCountValue ||
            options.limits.maxNodes - nodeCountValue - modelNodes < 1)
            throw error(name, 0, "NODE_LIMIT", "workspace node count exceeds configured limit");
        const auto materialOffset = output.model.materials.size();
        for (const auto& material : model.materials)
            for (const auto& resource : material.resources)
                if (resource.textureId != std::numeric_limits<std::uint32_t>::max() &&
                    static_cast<std::size_t>(resource.textureId) >= model.textures.size())
                    throw error(name, 0, "INVALID_REFERENCE", "material resource texture ID exceeds texture count");
        std::vector<std::uint32_t> textureRemap;
        textureRemap.reserve(model.textures.size());
        for (const auto& texture : model.textures) {
            textureBytes = checkedAdd(textureBytes, texture.size, name, "workspace texture");
            if (textureBytes > options.limits.maxTextureBytes)
                throw error(name, 0, "TEXTURE_SIZE_LIMIT", "workspace texture bytes exceed configured limit");
            const auto key = textureKey(texture.name);
            const auto previous = textureMap.find(key);
            if (previous != textureMap.end()) {
                const auto& previousRecord = output.workspace.textureRecords[previous->second];
                output.workspace.textureCollisions.push_back({texture.name, previousRecord.workspaceFile,
                                                              name, previousRecord.texture.size != texture.size});
            }
            if (output.workspace.scopeResources || previous == textureMap.end()) {
                if (output.model.textures.size() >= options.limits.maxTextures)
                    throw error(name, 0, "TEXTURE_LIMIT", "workspace texture count exceeds configured limit");
                if (output.model.textures.size() > std::numeric_limits<std::uint32_t>::max())
                    throw error(name, 0, "SIZE_OVERFLOW", "workspace texture ID overflows");
                output.model.textures.push_back(texture);
                output.workspace.textureRecords.push_back({texture, name, fileIndex});
                textureMap[key] = output.model.textures.size() - 1;
            } else {
                output.model.textures[previous->second] = texture;
                output.workspace.textureRecords[previous->second] = {texture, name, fileIndex};
                textureMap[key] = previous->second;
            }
            const auto mapped = textureMap[key];
            if (mapped > std::numeric_limits<std::uint32_t>::max())
                throw error(name, 0, "SIZE_OVERFLOW", "workspace texture ID overflows");
            textureRemap.push_back(static_cast<std::uint32_t>(mapped));
        }
        for (const auto& originalMaterial : model.materials) {
            auto material = originalMaterial;
            for (auto& resource : material.resources) {
                if (resource.textureId == std::numeric_limits<std::uint32_t>::max()) continue;
                if (static_cast<std::size_t>(resource.textureId) >= textureRemap.size())
                    throw error(name, 0, "INVALID_REFERENCE", "material resource texture ID exceeds texture count");
                resource.textureId = textureRemap[resource.textureId];
            }
            output.model.materials.push_back(std::move(material));
            output.workspace.materialRecords.push_back({output.model.materials.back(), name, fileIndex});
        }
        std::size_t remappedNodeCount = 0;
        auto root = remapNode(model.root, materialOffset, model.materials.size(), remappedNodeCount,
                              options.limits, name);
        Kn5Node wrapper;
        wrapper.type = 1;
        wrapper.kind = "node";
        wrapper.name = name;
        wrapper.active = true;
        wrapper.transform = modelPlacementMatrix(entry.position, entry.rotation);
        wrapper.children.push_back(std::move(root));
        output.model.root.children.push_back(std::move(wrapper));
        const auto withWrapper = checkedAdd(modelNodes, 1, name, "workspace node");
        nodeCountValue = checkedAdd(nodeCountValue, withWrapper, name, "workspace node");
        WorkspaceFile file;
        file.name = name;
        file.size = modelBytes;
        file.version = model.version;
        file.materialOffset = materialOffset;
        file.materials = model.materials.size();
        file.textures = model.textures.size();
        for (const auto& texture : model.textures) file.textureBytes = checkedAdd(file.textureBytes, texture.size, name, "file texture");
        file.position = entry.position;
        file.rotation = entry.rotation;
        file.lod = entry.lod;
        file.manifestIndex = entry.manifestIndex;
        file.auxiliary = entry.auxiliary;
        file.dynamic = entry.dynamic;
        file.protectedFile = model.encryption.has_value();
        output.workspace.files.push_back(std::move(file));
        if (model.encryption.has_value()) output.workspace.protectedFiles.push_back({name, *model.encryption});
        output.model.bytesRead = checkedAdd(output.model.bytesRead, model.bytesRead, name, "workspace bytes read");
        output.model.byteLength = checkedAdd(output.model.byteLength, model.byteLength, name, "workspace byte length");
        output.model.sourceMarker = std::max(output.model.sourceMarker, model.sourceMarker);
        output.model.version = std::max(output.model.version, model.version);
        versions.insert(model.version);
    }
    output.workspace.versions.assign(versions.begin(), versions.end());
    return output;
}

WorkspaceAssembly assembleTrackWorkspace(const TrackManifest& manifest,
                                         std::span<const WorkspaceModelInput> available,
                                         WorkspaceOptions options) {
    options.kind = "track";
    options.manifest = manifest.source;
    options.warnings.insert(options.warnings.end(), manifest.warnings.begin(), manifest.warnings.end());
    std::vector<bool> used(available.size(), false);
    std::vector<WorkspaceModelInput> entries;
    for (const auto& model : manifest.models) {
        const auto* input = matchAvailable(model.file, available, used, manifest.source);
        auto copy = *input;
        copy.name = model.file;
        copy.position = model.position;
        copy.rotation = model.rotation;
        copy.manifestIndex = model.index;
        copy.dynamic.reset();
        entries.push_back(std::move(copy));
    }
    auto dynamic = contiguousDynamicTrackObjects(manifest.dynamicObjects, options.warnings);
    for (const auto& object : dynamic) {
        const auto* input = matchAvailable(object.file, available, used, manifest.source);
        auto copy = *input;
        copy.name = object.file;
        copy.position = object.positionCenter;
        copy.dynamic = object;
        entries.push_back(std::move(copy));
    }
    for (std::size_t index = 0; index < available.size(); ++index)
        if (!used[index] && !available[index].auxiliary.empty()) entries.push_back(available[index]);
    if (entries.empty()) throw error(manifest.source, 0, "EMPTY_WORKSPACE", "track manifest has no supplied models");
    return mergeKn5Models(entries, std::move(options));
}

WorkspaceAssembly assembleCarLodWorkspace(const CarManifest& manifest,
                                           std::span<const WorkspaceModelInput> available,
                                           WorkspaceOptions options) {
    options.kind = "carLods";
    options.manifest = manifest.source;
    options.cockpitHrDistance = manifest.cockpitHrDistance;
    options.driverHrDistance = manifest.driverHrDistance;
    options.warnings.insert(options.warnings.end(), manifest.warnings.begin(), manifest.warnings.end());
    std::vector<bool> used(available.size(), false);
    std::vector<WorkspaceModelInput> entries;
    for (const auto& lod : manifest.lods) {
        if (lod.file.empty())
            throw error(manifest.source, lod.line, "INVALID_REFERENCE",
                        "car LOD has no safe model file reference");
        const auto* input = matchAvailable(lod.file, available, used, manifest.source);
        auto copy = *input;
        copy.name = lod.file;
        copy.lod = lod;
        entries.push_back(std::move(copy));
    }
    for (std::size_t index = 0; index < available.size(); ++index)
        if (!used[index] && !available[index].auxiliary.empty()) entries.push_back(available[index]);
    if (entries.empty()) throw error(manifest.source, 0, "EMPTY_WORKSPACE", "car manifest has no supplied LOD models");
    return mergeKn5Models(entries, std::move(options));
}

} // namespace apex::workspace
