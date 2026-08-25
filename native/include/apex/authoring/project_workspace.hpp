#pragma once

#include "apex/authoring/project.hpp"
#include "apex/workspace/workspace.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex::authoring {

enum class ProjectWorkspaceKind : std::uint8_t {
    trackModels,
    carLods,
};

enum class ProjectWorkspaceStatus : std::uint8_t {
    ok,
    invalid,
    failed,
};

struct ProjectWorkspaceDiagnostic {
    std::string code;
    std::string path;
    std::string message;
    std::size_t line = 0;
};

struct ProjectWorkspaceResult {
    ProjectWorkspaceStatus status = ProjectWorkspaceStatus::failed;
    std::vector<ProjectWorkspaceDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept {
        return status == ProjectWorkspaceStatus::ok;
    }
};

/** Owns an immutable parsed workspace baseline for one manifest category. */
class ProjectWorkspaceBaseline final {
public:
    ProjectWorkspaceBaseline(ProjectWorkspaceKind kind,
                             workspace::WorkspaceMetadata metadata)
        : kind_(kind), metadata_(std::move(metadata)) {}

    ProjectWorkspaceBaseline(ProjectWorkspaceBaseline&&) noexcept = default;
    ProjectWorkspaceBaseline& operator=(ProjectWorkspaceBaseline&&) noexcept = default;
    ProjectWorkspaceBaseline(const ProjectWorkspaceBaseline&) = default;
    ProjectWorkspaceBaseline& operator=(const ProjectWorkspaceBaseline&) = default;

    [[nodiscard]] ProjectWorkspaceKind kind() const noexcept { return kind_; }
    [[nodiscard]] const workspace::WorkspaceMetadata& value() const noexcept {
        return metadata_;
    }

private:
    ProjectWorkspaceKind kind_ = ProjectWorkspaceKind::trackModels;
    workspace::WorkspaceMetadata metadata_;
};

struct ProjectWorkspaceCaptureResult : ProjectWorkspaceResult {
    std::optional<ProjectWorkspaceBaseline> baseline;
};

struct ProjectWorkspaceApplyResult : ProjectWorkspaceResult {
    std::optional<workspace::WorkspaceMetadata> candidate;
    std::size_t applied = 0;
};

struct ProjectWorkspaceExportResult : ProjectWorkspaceResult {
    std::optional<workspace::WorkspaceMetadata> candidate;
    std::string suggested_name;
    std::string text;
    std::size_t applied = 0;
};

[[nodiscard]] ProjectWorkspaceCaptureResult captureProjectTrackWorkspaceBaseline(
    std::string_view source, std::span<const std::uint8_t> bytes,
    formats::IniParseLimits ini_limits = {},
    workspace::WorkspaceLimits limits = {});

[[nodiscard]] ProjectWorkspaceCaptureResult captureProjectCarLodWorkspaceBaseline(
    std::string_view source, std::span<const std::uint8_t> bytes,
    formats::IniParseLimits ini_limits = {},
    workspace::WorkspaceLimits limits = {});

/** Reset from baseline, apply positional project edits, and validate output. */
[[nodiscard]] ProjectWorkspaceApplyResult applyProjectWorkspaceEdits(
    const ProjectState& project, const ProjectWorkspaceBaseline& baseline,
    workspace::WorkspaceLimits limits = {});

/** Apply edits and return deterministic owned models.ini or data/lods.ini text. */
[[nodiscard]] ProjectWorkspaceExportResult exportProjectWorkspace(
    const ProjectState& project, const ProjectWorkspaceBaseline& baseline,
    workspace::WorkspaceLimits limits = {});

inline ProjectWorkspaceCaptureResult capture_project_track_workspace_baseline(
    std::string_view source, std::span<const std::uint8_t> bytes,
    formats::IniParseLimits ini_limits = {}, workspace::WorkspaceLimits limits = {}) {
    return captureProjectTrackWorkspaceBaseline(source, bytes, ini_limits, limits);
}

inline ProjectWorkspaceCaptureResult capture_project_car_lod_workspace_baseline(
    std::string_view source, std::span<const std::uint8_t> bytes,
    formats::IniParseLimits ini_limits = {}, workspace::WorkspaceLimits limits = {}) {
    return captureProjectCarLodWorkspaceBaseline(source, bytes, ini_limits, limits);
}

inline ProjectWorkspaceApplyResult apply_project_workspace_edits(
    const ProjectState& project, const ProjectWorkspaceBaseline& baseline,
    workspace::WorkspaceLimits limits = {}) {
    return applyProjectWorkspaceEdits(project, baseline, limits);
}

inline ProjectWorkspaceExportResult export_project_workspace(
    const ProjectState& project, const ProjectWorkspaceBaseline& baseline,
    workspace::WorkspaceLimits limits = {}) {
    return exportProjectWorkspace(project, baseline, limits);
}

}  // namespace apex::authoring
