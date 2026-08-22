#include "apex/authoring/project_surfaces.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::authoring::ProjectState;
using apex::authoring::SurfaceEdit;
using apex::domain::TrackDataLimits;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> baselineBytes() {
    const std::string text =
        "[SURFACE_4]\nKEY=ROAD\nFRICTION=0.98\nCUSTOM_ALPHA=one\n"
        "[SURFACE_9]\nKEY=KERB\nWAV=kerb.wav\nCUSTOM_BETA=two\n";
    return {text.begin(), text.end()};
}

void opensBaselineAndPreservesSourceShape() {
    const auto opened = apex::authoring::captureProjectSurfacesBaseline(
        "data/surfaces.ini", baselineBytes());
    require(opened.ok() && opened.baseline.has_value(),
            "surfaces baseline opens");
    require(opened.baseline->value().surfaces.size() == 2U &&
                opened.baseline->value().surfaces[0].index == 4U &&
                opened.baseline->value().surfaces[1].index == 9U,
            "sparse source section IDs are retained");
    require(opened.baseline->value().surfaces[0].fields.back().key == "CUSTOM_ALPHA" &&
                opened.baseline->value().surfaces[1].fields.back().key == "CUSTOM_BETA",
            "unknown fields are retained in source order");
}

void appliesDeterministicallyAndResetsFromBaseline() {
    const auto opened = apex::authoring::captureProjectSurfacesBaseline(
        "data/surfaces.ini", baselineBytes());
    require(opened.ok() && opened.baseline.has_value(), "baseline opens for edits");

    ProjectState project;
    SurfaceEdit first;
    first.key = "ROAD_EDITED";
    first.friction = 0.75F;
    first.isValidTrack = true;
    first.wav = "";
    project.surfaces.emplace(0U, first);
    SurfaceEdit second;
    second.ffEffect = "rumble";
    project.surfaces.emplace(1U, second);

    const auto firstExport = apex::authoring::exportProjectSurfaces(
        project, *opened.baseline);
    const auto secondExport = apex::authoring::exportProjectSurfaces(
        project, *opened.baseline);
    require(firstExport.ok() && firstExport.candidate.has_value() &&
                firstExport.text == secondExport.text && firstExport.applied == 2U,
            "surface export is deterministic and applies all edits");
    require(firstExport.text.find("[SURFACE_4]") != std::string::npos &&
                firstExport.text.find("KEY=ROAD_EDITED") != std::string::npos &&
                firstExport.text.find("CUSTOM_ALPHA=one") != std::string::npos &&
                firstExport.text.find("[SURFACE_9]") != std::string::npos &&
                firstExport.text.find("CUSTOM_BETA=two") != std::string::npos,
            "edited output preserves sparse IDs and unknown fields");
    require(opened.baseline->value().surfaces[0].key == "ROAD" &&
                opened.baseline->value().surfaces[0].friction == 0.98,
            "applying edits does not mutate the immutable baseline");

    const ProjectState reset;
    const auto resetExport = apex::authoring::exportProjectSurfaces(
        reset, *opened.baseline);
    const auto baselineExport = apex::domain::serialize_track_surfaces_ini(
        opened.baseline->value());
    require(resetExport.ok() && resetExport.text == baselineExport &&
                resetExport.applied == 0U,
            "empty edits reset output from the baseline");
}

void rejectsMissingAndInvalidEditsAtomically() {
    const auto opened = apex::authoring::captureProjectSurfacesBaseline(
        "data/surfaces.ini", baselineBytes());
    require(opened.ok() && opened.baseline.has_value(), "baseline opens for rejection tests");

    ProjectState missing;
    SurfaceEdit missingEdit;
    missingEdit.key = "MISSING";
    missing.surfaces.emplace(2U, missingEdit);
    const auto missingResult = apex::authoring::applyProjectSurfaceEdits(
        missing, *opened.baseline);
    require(!missingResult.ok() && !missingResult.candidate.has_value() &&
                missingResult.applied == 0U &&
                missingResult.diagnostics.back().code == "MISSING_SURFACE_POSITION",
            "missing position rejects without a partial candidate");

    ProjectState nonfinite;
    SurfaceEdit nanEdit;
    nanEdit.friction = std::numeric_limits<float>::quiet_NaN();
    nonfinite.surfaces.emplace(0U, nanEdit);
    const auto nonfiniteResult = apex::authoring::applyProjectSurfaceEdits(
        nonfinite, *opened.baseline);
    require(!nonfiniteResult.ok() && !nonfiniteResult.candidate.has_value() &&
                nonfiniteResult.diagnostics.back().code == "NON_FINITE_VALUE",
            "non-finite edit rejects atomically");

    ProjectState unsafe;
    SurfaceEdit unsafeEdit;
    unsafeEdit.wav = "bad\nname.wav";
    unsafe.surfaces.emplace(0U, unsafeEdit);
    const auto unsafeResult = apex::authoring::applyProjectSurfaceEdits(
        unsafe, *opened.baseline);
    require(!unsafeResult.ok() && !unsafeResult.candidate.has_value() &&
                unsafeResult.diagnostics.back().code == "UNSAFE_TEXT",
            "unsafe text rejects atomically");
}

void rejectsMalformedAndOutputLimitedInput() {
    const std::vector<std::uint8_t> malformed{
        '[', 'S', 'U', 'R', 'F', 'A', 'C', 'E', '_', '0', '\n', 'K', 'E', 'Y', '='};
    const auto malformedResult = apex::authoring::captureProjectSurfacesBaseline(
        "malformed.ini", malformed);
    require(!malformedResult.ok() && !malformedResult.baseline.has_value(),
            "malformed input does not produce a partial baseline");

    const std::string truncatedText = "[SURFACE_0]\nKEY=ROAD\\";
    const std::vector<std::uint8_t> truncated(
        truncatedText.begin(), truncatedText.end());
    const auto truncatedResult = apex::authoring::captureProjectSurfacesBaseline(
        "truncated.ini", truncated);
    require(!truncatedResult.ok() && !truncatedResult.baseline.has_value() &&
                !truncatedResult.diagnostics.empty() &&
                truncatedResult.diagnostics.back().code == "TRUNCATED_CONTINUATION",
            "truncated continuation fails with source diagnostics");

    const auto opened = apex::authoring::captureProjectSurfacesBaseline(
        "data/surfaces.ini", baselineBytes());
    require(opened.ok() && opened.baseline.has_value(), "baseline opens for output limit");
    TrackDataLimits limits;
    limits.maxOutputBytes = 16U;
    const auto limited = apex::authoring::exportProjectSurfaces(
        ProjectState{}, *opened.baseline, limits);
    require(!limited.ok() && !limited.candidate.has_value() && limited.text.empty() &&
                limited.diagnostics.back().code == "OUTPUT_LIMIT",
            "output limit rejects without returning partial text or candidate");
}

}  // namespace

int main() {
    try {
        opensBaselineAndPreservesSourceShape();
        appliesDeterministicallyAndResetsFromBaseline();
        rejectsMissingAndInvalidEditsAtomically();
        rejectsMalformedAndOutputLimitedInput();
        std::cout << "project_surfaces_tests: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "project_surfaces_tests: " << error.what() << '\n';
        return 1;
    }
}
