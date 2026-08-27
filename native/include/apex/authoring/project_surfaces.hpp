#pragma once

#include "apex/authoring/project.hpp"
#include "apex/domain/track_data.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex::authoring {

/** Status returned by the bounded surfaces authoring adapter. */
enum class ProjectSurfacesStatus : std::uint8_t {
    ok,
    invalid,
    failed,
};

struct ProjectSurfacesDiagnostic {
    domain::TrackDiagnosticSeverity severity = domain::TrackDiagnosticSeverity::error;
    std::string code;
    std::string path;
    std::string message;
    std::size_t line = 0;
};

struct ProjectSurfacesResult {
    ProjectSurfacesStatus status = ProjectSurfacesStatus::failed;
    std::vector<ProjectSurfacesDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept {
        return status == ProjectSurfacesStatus::ok;
    }
};

/**
 * Owns a parsed surfaces.ini baseline. The accessor is const so callers can
 * only derive candidates; edits never mutate the retained baseline.
 */
class ProjectSurfacesBaseline final {
public:
    explicit ProjectSurfacesBaseline(domain::TrackSurfaces surfaces)
        : surfaces_(std::move(surfaces)) {}

    ProjectSurfacesBaseline(ProjectSurfacesBaseline&&) noexcept = default;
    ProjectSurfacesBaseline& operator=(ProjectSurfacesBaseline&&) noexcept = default;
    ProjectSurfacesBaseline(const ProjectSurfacesBaseline&) = default;
    ProjectSurfacesBaseline& operator=(const ProjectSurfacesBaseline&) = default;

    [[nodiscard]] const domain::TrackSurfaces& value() const noexcept {
        return surfaces_;
    }

private:
    domain::TrackSurfaces surfaces_;
};

struct ProjectSurfacesCaptureResult : ProjectSurfacesResult {
    std::optional<ProjectSurfacesBaseline> baseline;
};

struct ProjectSurfacesApplyResult : ProjectSurfacesResult {
    std::optional<domain::TrackSurfaces> candidate;
    std::size_t applied = 0;
};

struct ProjectSurfacesExportResult : ProjectSurfacesResult {
    std::optional<domain::TrackSurfaces> candidate;
    std::string text;
    std::size_t applied = 0;
};

/** Parse and retain an owned immutable baseline from caller-supplied bytes. */
[[nodiscard]] ProjectSurfacesCaptureResult captureProjectSurfacesBaseline(
    std::string_view source, std::span<const std::uint8_t> bytes,
    domain::TrackDataLimits limits = {});

/**
 * Reset from the baseline, then apply positional surface edits exactly once.
 * Any invalid position, value, or output-limit failure rejects the complete
 * candidate; no partially edited result is returned.
 */
[[nodiscard]] ProjectSurfacesApplyResult applyProjectSurfaceEdits(
    const ProjectState& project, const ProjectSurfacesBaseline& baseline,
    domain::TrackDataLimits limits = {});

/** Apply and serialize a complete candidate with owned text output. */
[[nodiscard]] ProjectSurfacesExportResult exportProjectSurfaces(
    const ProjectState& project, const ProjectSurfacesBaseline& baseline,
    domain::TrackDataLimits limits = {});

inline ProjectSurfacesCaptureResult capture_project_surfaces_baseline(
    std::string_view source, std::span<const std::uint8_t> bytes,
    domain::TrackDataLimits limits = {}) {
    return captureProjectSurfacesBaseline(source, bytes, limits);
}

inline ProjectSurfacesApplyResult apply_project_surface_edits(
    const ProjectState& project, const ProjectSurfacesBaseline& baseline,
    domain::TrackDataLimits limits = {}) {
    return applyProjectSurfaceEdits(project, baseline, limits);
}

inline ProjectSurfacesExportResult export_project_surfaces(
    const ProjectState& project, const ProjectSurfacesBaseline& baseline,
    domain::TrackDataLimits limits = {}) {
    return exportProjectSurfaces(project, baseline, limits);
}

}  // namespace apex::authoring
