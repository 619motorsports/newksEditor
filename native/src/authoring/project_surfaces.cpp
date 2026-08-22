#include "apex/authoring/project_surfaces.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace apex::authoring {
namespace {

[[nodiscard]] ProjectSurfacesDiagnostic diagnostic(
    domain::TrackDiagnosticSeverity severity, std::string_view code,
    std::string_view path, std::string_view message, std::size_t line = 0) {
    return {severity, std::string(code), std::string(path), std::string(message), line};
}

void addTrackDiagnostics(ProjectSurfacesResult& result,
                         const std::vector<domain::TrackDiagnostic>& diagnostics) {
    for (const auto& item : diagnostics) {
        result.diagnostics.push_back(diagnostic(
            item.severity, item.code, item.source, item.message, item.line));
    }
}

void addError(ProjectSurfacesResult& result, std::string_view code,
              std::string_view path, std::string_view message, std::size_t line = 0) {
    result.status = ProjectSurfacesStatus::invalid;
    result.diagnostics.push_back(
        diagnostic(domain::TrackDiagnosticSeverity::error, code, path, message, line));
}

[[nodiscard]] std::string upperAscii(std::string_view value) {
    std::string output(value);
    for (auto& character : output) {
        if (character >= 'a' && character <= 'z')
            character = static_cast<char>(character - ('a' - 'A'));
    }
    return output;
}

[[nodiscard]] bool hasEdit(const SurfaceEdit& edit) noexcept {
    return edit.key || edit.friction || edit.damping || edit.dirtAdditive ||
           edit.blackFlagTime || edit.isValidTrack || edit.isPitlane || edit.sinHeight ||
           edit.sinLength || edit.vibrationGain || edit.vibrationLength || edit.wav ||
           edit.wavPitch || edit.ffEffect;
}

template <typename T>
[[nodiscard]] bool finiteOptional(const std::optional<T>& value) noexcept {
    return !value || std::isfinite(static_cast<double>(*value));
}

[[nodiscard]] bool finiteEdit(const SurfaceEdit& edit) noexcept {
    return finiteOptional(edit.friction) && finiteOptional(edit.damping) &&
           finiteOptional(edit.dirtAdditive) && finiteOptional(edit.blackFlagTime) &&
           finiteOptional(edit.sinHeight) && finiteOptional(edit.sinLength) &&
           finiteOptional(edit.vibrationGain) && finiteOptional(edit.vibrationLength) &&
           finiteOptional(edit.wavPitch);
}

void applyEdit(domain::TrackSurface& surface, const SurfaceEdit& edit) {
    if (edit.key) {
        surface.key = *edit.key;
        surface.normalized_key = upperAscii(*edit.key);
    }
    if (edit.friction) surface.friction = *edit.friction;
    if (edit.damping) surface.damping = *edit.damping;
    if (edit.dirtAdditive) surface.dirt_additive = *edit.dirtAdditive;
    if (edit.blackFlagTime) surface.black_flag_time = *edit.blackFlagTime;
    if (edit.isValidTrack) surface.is_valid_track = *edit.isValidTrack;
    if (edit.isPitlane) surface.is_pitlane = *edit.isPitlane;
    if (edit.sinHeight) surface.sin_height = *edit.sinHeight;
    if (edit.sinLength) surface.sin_length = *edit.sinLength;
    if (edit.vibrationGain) surface.vibration_gain = *edit.vibrationGain;
    if (edit.vibrationLength) surface.vibration_length = *edit.vibrationLength;
    if (edit.wav) surface.wav = *edit.wav;
    if (edit.wavPitch) surface.wav_pitch = *edit.wavPitch;
    if (edit.ffEffect) surface.ff_effect = *edit.ffEffect;
}

template <typename Result>
void addException(Result& result, std::string_view code, std::string_view path,
                  const std::exception& error) {
    result.status = ProjectSurfacesStatus::failed;
    result.diagnostics.push_back(diagnostic(
        domain::TrackDiagnosticSeverity::error, code, path, error.what()));
}

}  // namespace

ProjectSurfacesCaptureResult captureProjectSurfacesBaseline(
    std::string_view source, std::span<const std::uint8_t> bytes,
    domain::TrackDataLimits limits) {
    ProjectSurfacesCaptureResult result;
    try {
        const auto text = bytes.empty()
                              ? std::string_view{}
                              : std::string_view(
                                    reinterpret_cast<const char*>(bytes.data()), bytes.size());
        auto parsed = domain::parse_track_surfaces(
            text, std::string(source), limits);
        addTrackDiagnostics(result, parsed.diagnostics);
        if (parsed.surfaces.empty()) {
            addError(result, "EMPTY_SURFACE_MANIFEST", source,
                     "surfaces input contains no SURFACE_n sections");
            return result;
        }
        if (parsed.has_errors()) {
            result.status = ProjectSurfacesStatus::invalid;
            return result;
        }
        result.baseline.emplace(std::move(parsed));
        result.status = ProjectSurfacesStatus::ok;
    } catch (const core::ParseError& error) {
        result.status = ProjectSurfacesStatus::invalid;
        result.diagnostics.push_back(diagnostic(
            domain::TrackDiagnosticSeverity::error, error.code(), error.source(),
            error.what(), error.offset()));
    } catch (const std::exception& error) {
        addException(result, "SURFACES_CAPTURE_FAILED", source, error);
    }
    return result;
}

ProjectSurfacesApplyResult applyProjectSurfaceEdits(
    const ProjectState& project, const ProjectSurfacesBaseline& baseline,
    domain::TrackDataLimits limits) {
    ProjectSurfacesApplyResult result;
    result.diagnostics.reserve(baseline.value().diagnostics.size());
    addTrackDiagnostics(result, baseline.value().diagnostics);
    auto candidate = baseline.value();
    std::size_t applied = 0;
    for (const auto& [position, edit] : project.surfaces) {
        const auto path = "surfaceEdits." + std::to_string(position);
        if (position >= candidate.surfaces.size()) {
            addError(result, "MISSING_SURFACE_POSITION", path,
                     "surface edit position does not exist in the baseline");
            return result;
        }
        if (!hasEdit(edit)) {
            addError(result, "EDIT_EMPTY", path, "surface edit has no fields");
            return result;
        }
        if (!finiteEdit(edit)) {
            addError(result, "NON_FINITE_VALUE", path,
                     "surface edit contains a non-finite number");
            return result;
        }
        applyEdit(candidate.surfaces[position], edit);
        ++applied;
    }

    try {
        (void)domain::serialize_track_surfaces_ini(candidate, limits);
        result.candidate = std::move(candidate);
        result.applied = applied;
        result.status = ProjectSurfacesStatus::ok;
    } catch (const core::ParseError& error) {
        result.status = ProjectSurfacesStatus::invalid;
        result.diagnostics.push_back(diagnostic(
            domain::TrackDiagnosticSeverity::error, error.code(), error.source(),
            error.what(), error.offset()));
        result.applied = 0;
    } catch (const std::exception& error) {
        addException(result, "SURFACES_APPLY_FAILED", baseline.value().source, error);
        result.applied = 0;
    }
    return result;
}

ProjectSurfacesExportResult exportProjectSurfaces(
    const ProjectState& project, const ProjectSurfacesBaseline& baseline,
    domain::TrackDataLimits limits) {
    ProjectSurfacesExportResult result;
    const auto applied = applyProjectSurfaceEdits(project, baseline, limits);
    result.status = applied.status;
    result.diagnostics = applied.diagnostics;
    result.applied = applied.applied;
    if (!applied.ok() || !applied.candidate) return result;
    try {
        result.text = domain::serialize_track_surfaces_ini(*applied.candidate, limits);
        result.candidate = *applied.candidate;
    } catch (const core::ParseError& error) {
        result.status = ProjectSurfacesStatus::invalid;
        result.text.clear();
        result.candidate.reset();
        result.applied = 0;
        result.diagnostics.push_back(diagnostic(
            domain::TrackDiagnosticSeverity::error, error.code(), error.source(),
            error.what(), error.offset()));
    } catch (const std::exception& error) {
        result.status = ProjectSurfacesStatus::failed;
        result.text.clear();
        result.candidate.reset();
        result.applied = 0;
        result.diagnostics.push_back(diagnostic(
            domain::TrackDiagnosticSeverity::error, "SURFACES_EXPORT_FAILED",
            baseline.value().source, error.what()));
    }
    return result;
}

}  // namespace apex::authoring
