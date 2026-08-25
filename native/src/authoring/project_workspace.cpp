#include "apex/authoring/project_workspace.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace apex::authoring {
namespace {

[[nodiscard]] ProjectWorkspaceDiagnostic diagnostic(
    std::string_view code, std::string_view path, std::string_view message,
    std::size_t line = 0) {
    return {std::string(code), std::string(path), std::string(message), line};
}

void addWarnings(ProjectWorkspaceResult& result,
                 const std::vector<std::string>& warnings) {
    for (const auto& warning : warnings)
        result.diagnostics.push_back(
            diagnostic("MANIFEST_WARNING", "manifest", warning));
}

void addError(ProjectWorkspaceResult& result, std::string_view code,
              std::string_view path, std::string_view message,
              std::size_t line = 0) {
    result.status = ProjectWorkspaceStatus::invalid;
    result.diagnostics.push_back(diagnostic(code, path, message, line));
}

template <typename Result>
void addException(Result& result, std::string_view code, std::string_view path,
                  const std::exception& error) {
    result.status = ProjectWorkspaceStatus::failed;
    result.diagnostics.push_back(diagnostic(code, path, error.what()));
}

[[nodiscard]] std::string textBytes(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] bool hasEdit(const WorkspaceFileEdit& edit) noexcept {
    return edit.name || edit.position || edit.rotation || edit.lodIn || edit.lodOut ||
           edit.probability || edit.multiplicity || edit.posMode || edit.positionCenter ||
           edit.positionRange || edit.velMode || edit.velocityBase || edit.velocityRange ||
           edit.playWav;
}

template <typename T>
[[nodiscard]] bool finiteOptional(const std::optional<T>& value) noexcept {
    return !value || std::isfinite(static_cast<double>(*value));
}

template <std::size_t N>
[[nodiscard]] bool finiteVector(const std::optional<std::array<float, N>>& value) noexcept {
    if (!value) return true;
    return std::all_of(value->begin(), value->end(),
                       [](float item) { return std::isfinite(item); });
}

[[nodiscard]] bool safeText(std::string_view value, std::size_t maximum,
                            bool allow_empty = false) noexcept {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](char item) {
        const auto byte = static_cast<unsigned char>(item);
        return byte >= 0x20U && byte != 0x7fU;
    });
}

[[nodiscard]] bool safeManifestFile(std::string_view value,
                                    std::size_t maximum) noexcept {
    if (!safeText(value, maximum)) return false;
    if (value.front() == '/' || (value.size() >= 2U &&
                                 ((value[0] >= 'A' && value[0] <= 'Z') ||
                                  (value[0] >= 'a' && value[0] <= 'z')) &&
                                 value[1] == ':'))
        return false;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find_first_of("/\\", begin);
        const auto part = value.substr(
            begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return true;
}

[[nodiscard]] bool validEdit(const WorkspaceFileEdit& edit,
                             std::size_t max_string_bytes) noexcept {
    if (edit.name && !safeManifestFile(*edit.name, max_string_bytes)) return false;
    if (edit.posMode && !safeText(*edit.posMode, max_string_bytes)) return false;
    if (edit.velMode && !safeText(*edit.velMode, max_string_bytes)) return false;
    if (edit.playWav && !safeText(*edit.playWav, max_string_bytes, true)) return false;
    return finiteOptional(edit.lodIn) && finiteOptional(edit.lodOut) &&
           finiteOptional(edit.probability) && finiteVector(edit.position) &&
           finiteVector(edit.rotation) && finiteVector(edit.positionCenter) &&
           finiteVector(edit.positionRange) && finiteVector(edit.velocityBase) &&
           finiteVector(edit.velocityRange) && finiteVector(edit.multiplicity);
}

[[nodiscard]] workspace::WorkspaceMetadata trackMetadata(
    const workspace::TrackManifest& parsed) {
    workspace::WorkspaceMetadata metadata;
    metadata.kind = "track";
    metadata.manifest = parsed.source;
    metadata.warnings = parsed.warnings;
    metadata.files.reserve(parsed.models.size() + parsed.dynamicObjects.size());
    for (const auto& model : parsed.models) {
        workspace::WorkspaceFile file;
        file.name = model.file;
        file.position = model.position;
        file.rotation = model.rotation;
        file.manifestIndex = model.index;
        metadata.files.push_back(std::move(file));
    }
    for (const auto& object : parsed.dynamicObjects) {
        workspace::WorkspaceFile file;
        file.name = object.file;
        file.position = object.positionCenter;
        file.dynamic = object;
        metadata.files.push_back(std::move(file));
    }
    return metadata;
}

[[nodiscard]] workspace::WorkspaceMetadata carMetadata(
    const workspace::CarManifest& parsed) {
    workspace::WorkspaceMetadata metadata;
    metadata.kind = "carLods";
    metadata.manifest = parsed.source;
    metadata.warnings = parsed.warnings;
    metadata.cockpitHrDistance = parsed.cockpitHrDistance;
    metadata.driverHrDistance = parsed.driverHrDistance;
    metadata.files.reserve(parsed.lods.size());
    for (const auto& lod : parsed.lods) {
        workspace::WorkspaceFile file;
        file.name = lod.file;
        file.lod = lod;
        metadata.files.push_back(std::move(file));
    }
    return metadata;
}

void applyEdit(workspace::WorkspaceFile& file, const WorkspaceFileEdit& edit) {
    if (file.dynamic) {
        if (edit.multiplicity) file.dynamic->multiplicity = *edit.multiplicity;
        if (edit.probability) file.dynamic->probability = *edit.probability;
        if (edit.posMode) file.dynamic->posMode = *edit.posMode;
        if (edit.positionCenter) file.dynamic->positionCenter = *edit.positionCenter;
        if (edit.positionRange) file.dynamic->positionRange = *edit.positionRange;
        if (edit.velMode) file.dynamic->velMode = *edit.velMode;
        if (edit.velocityBase) file.dynamic->velocityBase = *edit.velocityBase;
        if (edit.velocityRange) file.dynamic->velocityRange = *edit.velocityRange;
        if (edit.playWav) file.dynamic->playWav = *edit.playWav;
        file.position = file.dynamic->positionCenter;
    } else {
        if (edit.position) file.position = *edit.position;
        if (edit.rotation) file.rotation = *edit.rotation;
    }
    if (file.lod) {
        if (edit.name) file.name = *edit.name;
        if (edit.lodIn) file.lod->inDistance = *edit.lodIn;
        if (edit.lodOut) file.lod->outDistance = *edit.lodOut;
    }
}

[[nodiscard]] ProjectWorkspaceCaptureResult capture(
    ProjectWorkspaceKind kind, std::string_view source,
    std::span<const std::uint8_t> bytes, formats::IniParseLimits ini_limits,
    workspace::WorkspaceLimits limits) {
    ProjectWorkspaceCaptureResult result;
    try {
        const auto text = textBytes(bytes);
        workspace::WorkspaceMetadata metadata;
        if (kind == ProjectWorkspaceKind::trackModels) {
            const auto parsed = workspace::parseModelsIni(
                text, std::string(source), ini_limits, limits);
            addWarnings(result, parsed.warnings);
            if (!parsed.warnings.empty()) {
                addError(result, "MANIFEST_INVALID", source,
                         "workspace manifest contains rejected parser diagnostics");
                return result;
            }
            metadata = trackMetadata(parsed);
        } else {
            const auto parsed = workspace::parseCarLodsIni(
                text, std::string(source), ini_limits, limits);
            addWarnings(result, parsed.warnings);
            if (!parsed.warnings.empty()) {
                addError(result, "MANIFEST_INVALID", source,
                         "workspace manifest contains rejected parser diagnostics");
                return result;
            }
            metadata = carMetadata(parsed);
        }
        if (metadata.files.empty()) {
            addError(result, "EMPTY_WORKSPACE", source,
                     "workspace manifest contains no usable files");
            return result;
        }
        if (kind == ProjectWorkspaceKind::trackModels)
            (void)workspace::serializeModelsIni(metadata, limits);
        else
            (void)workspace::serializeCarLodsIni(metadata, limits);
        result.baseline.emplace(kind, std::move(metadata));
        result.status = ProjectWorkspaceStatus::ok;
    } catch (const core::ParseError& error) {
        result.status = ProjectWorkspaceStatus::invalid;
        result.diagnostics.push_back(diagnostic(
            error.code(), error.source(), error.what(), error.offset()));
    } catch (const std::exception& error) {
        addException(result, "WORKSPACE_CAPTURE_FAILED", source, error);
    }
    return result;
}

}  // namespace

ProjectWorkspaceCaptureResult captureProjectTrackWorkspaceBaseline(
    std::string_view source, std::span<const std::uint8_t> bytes,
    formats::IniParseLimits ini_limits, workspace::WorkspaceLimits limits) {
    return capture(ProjectWorkspaceKind::trackModels, source, bytes, ini_limits, limits);
}

ProjectWorkspaceCaptureResult captureProjectCarLodWorkspaceBaseline(
    std::string_view source, std::span<const std::uint8_t> bytes,
    formats::IniParseLimits ini_limits, workspace::WorkspaceLimits limits) {
    return capture(ProjectWorkspaceKind::carLods, source, bytes, ini_limits, limits);
}

ProjectWorkspaceApplyResult applyProjectWorkspaceEdits(
    const ProjectState& project, const ProjectWorkspaceBaseline& baseline,
    workspace::WorkspaceLimits limits) {
    ProjectWorkspaceApplyResult result;
    addWarnings(result, baseline.value().warnings);
    if (!finiteOptional(project.workspace.cockpitHrDistance) ||
        !finiteOptional(project.workspace.driverHrDistance)) {
        addError(result, "NON_FINITE_VALUE", "workspaceEdits",
                 "workspace distance contains a non-finite number");
        return result;
    }
    auto candidate = baseline.value();
    std::size_t applied = 0U;
    const bool car_lods = baseline.kind() == ProjectWorkspaceKind::carLods;
    candidate.cockpitHrDistance = project.workspace.cockpitHrDistance
                                      ? project.workspace.cockpitHrDistance
                                      : baseline.value().cockpitHrDistance;
    candidate.driverHrDistance = project.workspace.driverHrDistance
                                     ? project.workspace.driverHrDistance
                                     : baseline.value().driverHrDistance;
    const auto workspaceSetting = [&](bool present, std::string_view field) {
        if (!present) return;
        if (car_lods) {
            ++applied;
        } else {
            result.diagnostics.push_back(diagnostic(
                "UNSUPPORTED_FIELD", "workspaceEdits." + std::string(field),
                "track models.ini output does not contain car LOD settings"));
        }
    };
    workspaceSetting(project.workspace.cockpitHrDistance.has_value(),
                     "cockpitHrDistance");
    workspaceSetting(project.workspace.driverHrDistance.has_value(),
                     "driverHrDistance");
    for (const auto& [position, edit] : project.workspaceFiles) {
        const auto path = "workspaceEdits.files." + std::to_string(position);
        if (position >= candidate.files.size()) {
            addError(result, "MISSING_WORKSPACE_POSITION", path,
                     "workspace edit position does not exist in the baseline");
            return result;
        }
        if (!hasEdit(edit)) {
            addError(result, "EDIT_EMPTY", path, "workspace edit has no fields");
            return result;
        }
        if (!validEdit(edit, limits.maxPathBytes)) {
            addError(result, "INVALID_EDIT", path,
                     "workspace edit contains unsafe text or a non-finite value");
            return result;
        }
        const auto& file = candidate.files[position];
        const bool dynamic = file.dynamic.has_value();
        const bool lod = file.lod.has_value();
        const auto field = [&](bool present, bool supported,
                               std::string_view name) {
            if (!present) return;
            if (supported) {
                ++applied;
            } else {
                result.diagnostics.push_back(diagnostic(
                    "UNSUPPORTED_FIELD", path + "." + std::string(name),
                    "this workspace entry does not export the edited field"));
            }
        };
        field(edit.name.has_value(), lod, "name");
        field(edit.position.has_value(), !dynamic && !lod, "position");
        field(edit.rotation.has_value(), !dynamic && !lod, "rotation");
        field(edit.lodIn.has_value(), lod, "lodIn");
        field(edit.lodOut.has_value(), lod, "lodOut");
        field(edit.probability.has_value(), dynamic, "probability");
        field(edit.multiplicity.has_value(), dynamic, "multiplicity");
        field(edit.posMode.has_value(), dynamic, "posMode");
        field(edit.positionCenter.has_value(), dynamic, "positionCenter");
        field(edit.positionRange.has_value(), dynamic, "positionRange");
        field(edit.velMode.has_value(), dynamic, "velMode");
        field(edit.velocityBase.has_value(), dynamic, "velocityBase");
        field(edit.velocityRange.has_value(), dynamic, "velocityRange");
        field(edit.playWav.has_value(), dynamic, "playWav");
        applyEdit(candidate.files[position], edit);
    }
    try {
        if (baseline.kind() == ProjectWorkspaceKind::trackModels)
            (void)workspace::serializeModelsIni(candidate, limits);
        else
            (void)workspace::serializeCarLodsIni(candidate, limits);
        result.applied = applied;
        result.candidate = std::move(candidate);
        result.status = ProjectWorkspaceStatus::ok;
    } catch (const core::ParseError& error) {
        result.status = ProjectWorkspaceStatus::invalid;
        result.diagnostics.push_back(diagnostic(
            error.code(), error.source(), error.what(), error.offset()));
        result.applied = 0;
    } catch (const std::exception& error) {
        addException(result, "WORKSPACE_APPLY_FAILED", baseline.value().manifest, error);
        result.applied = 0;
    }
    return result;
}

ProjectWorkspaceExportResult exportProjectWorkspace(
    const ProjectState& project, const ProjectWorkspaceBaseline& baseline,
    workspace::WorkspaceLimits limits) {
    ProjectWorkspaceExportResult result;
    const auto applied = applyProjectWorkspaceEdits(project, baseline, limits);
    result.status = applied.status;
    result.diagnostics = applied.diagnostics;
    result.applied = applied.applied;
    if (!applied.ok() || !applied.candidate) return result;
    result.suggested_name = baseline.kind() == ProjectWorkspaceKind::trackModels
                                ? "models.ini"
                                : "lods.ini";
    try {
        result.text = baseline.kind() == ProjectWorkspaceKind::trackModels
                          ? workspace::serializeModelsIni(*applied.candidate, limits)
                          : workspace::serializeCarLodsIni(*applied.candidate, limits);
        result.candidate = *applied.candidate;
    } catch (const core::ParseError& error) {
        result.status = ProjectWorkspaceStatus::invalid;
        result.text.clear();
        result.candidate.reset();
        result.applied = 0;
        result.diagnostics.push_back(diagnostic(
            error.code(), error.source(), error.what(), error.offset()));
    } catch (const std::exception& error) {
        result.status = ProjectWorkspaceStatus::failed;
        result.text.clear();
        result.candidate.reset();
        result.applied = 0;
        result.diagnostics.push_back(diagnostic(
            "WORKSPACE_EXPORT_FAILED", baseline.value().manifest, error.what()));
    }
    return result;
}

}  // namespace apex::authoring
