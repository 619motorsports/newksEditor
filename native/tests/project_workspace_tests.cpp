#include "apex/authoring/project_workspace.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::authoring::ProjectState;
using apex::authoring::WorkspaceFileEdit;
using apex::workspace::WorkspaceLimits;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> trackBytes() {
    const std::string text =
        "[MODEL_4]\nFILE=main.kn5\nPOSITION=1, 2, 3\nROTATION=4, 5, 6\n"
        "[DYNAMIC_OBJECT_9]\nFILE=tree.kn5\nPROBABILITY=75\nMULT=1, 3\n"
        "POS_MODE=RANDOM\nRND_POS_CENTER=10, 20, 30\nRND_POS_RANGE=1, 2, 3\n"
        "VEL_MODE=RANDOM\nRND_VEL_BASE=4, 5, 6\nRND_VEL_RANGE=7, 8, 9\nPLAY_WAV=tree.wav\n";
    return {text.begin(), text.end()};
}

std::vector<std::uint8_t> carBytes() {
    const std::string text =
        "[COCKPIT_HR]\nDISTANCE_SWITCH=25\n"
        "[DRIVER_HR]\nDISTANCE_SWITCH=30\n"
        "[LOD_0]\nFILE=car.kn5\nIN=0\nOUT=15\n"
        "[LOD_1]\nFILE=car_lod_b.kn5\nIN=15\nOUT=45\n";
    return {text.begin(), text.end()};
}

void capturesSparseTrackBaseline() {
    const auto captured = apex::authoring::captureProjectTrackWorkspaceBaseline(
        "models.ini", trackBytes());
    require(captured.ok() && captured.baseline.has_value(),
            "track workspace baseline opens");
    const auto& files = captured.baseline->value().files;
    require(files.size() == 2U && files[0].manifestIndex == 4U &&
                files[1].dynamic.has_value() && files[1].dynamic->index == 9U,
            "track sparse model and dynamic indices are retained");
}

void appliesTrackStaticAndDynamicEditsDeterministically() {
    const auto captured = apex::authoring::captureProjectTrackWorkspaceBaseline(
        "models.ini", trackBytes());
    require(captured.ok() && captured.baseline.has_value(),
            "track baseline opens for edits");

    ProjectState project;
    WorkspaceFileEdit staticEdit;
    staticEdit.position = std::array<float, 3>{11, 12, 13};
    staticEdit.rotation = std::array<float, 3>{21, 22, 23};
    project.workspaceFiles.emplace(0U, staticEdit);
    WorkspaceFileEdit dynamicEdit;
    dynamicEdit.probability = 42.0F;
    dynamicEdit.positionCenter = std::array<float, 3>{30, 40, 50};
    dynamicEdit.playWav = "changed.wav";
    project.workspaceFiles.emplace(1U, dynamicEdit);

    const auto first = apex::authoring::exportProjectWorkspace(
        project, *captured.baseline);
    const auto second = apex::authoring::exportProjectWorkspace(
        project, *captured.baseline);
    require(first.ok() && second.ok() && first.text == second.text &&
                first.applied == 5U && first.suggested_name == "models.ini",
            "track export is deterministic and counts all edit fields");
    require(first.text.find("[MODEL_4]") != std::string::npos &&
                first.text.find("POSITION=11, 12, 13") != std::string::npos &&
                first.text.find("[DYNAMIC_OBJECT_9]") != std::string::npos &&
                first.text.find("PROBABILITY=42") != std::string::npos &&
                first.text.find("RND_POS_CENTER=30, 40, 50") != std::string::npos &&
                first.text.find("PLAY_WAV=changed.wav") != std::string::npos,
            "track output applies static and dynamic fields with sparse IDs");
    require(captured.baseline->value().files[0].position ==
                std::array<float, 3>{1, 2, 3} &&
                captured.baseline->value().files[1].dynamic->probability == 75.0F,
            "track edits do not mutate the immutable baseline");

    const auto reset = apex::authoring::exportProjectWorkspace(
        ProjectState{}, *captured.baseline);
    require(reset.ok() && reset.text == apex::workspace::serializeModelsIni(
                captured.baseline->value()),
            "track empty edits reset from baseline");
}

void appliesCarLodEditsAndSettings() {
    const auto captured = apex::authoring::captureProjectCarLodWorkspaceBaseline(
        "data/lods.ini", carBytes());
    require(captured.ok() && captured.baseline.has_value(),
            "car LOD baseline opens");
    require(captured.baseline->value().files.size() == 2U &&
                captured.baseline->value().files[1].lod->index == 1U,
            "car LOD positions retain source indices");

    ProjectState project;
    project.workspace.cockpitHrDistance = 35.0F;
    project.workspace.driverHrDistance = 40.0F;
    WorkspaceFileEdit lodEdit;
    lodEdit.name = "car_lod_b_edited.kn5";
    lodEdit.lodIn = 16.0F;
    lodEdit.lodOut = 50.0F;
    project.workspaceFiles.emplace(1U, lodEdit);

    const auto exported = apex::authoring::exportProjectWorkspace(
        project, *captured.baseline);
    require(exported.ok() && exported.suggested_name == "lods.ini" &&
                exported.text.find("DISTANCE_SWITCH=35") != std::string::npos &&
                exported.text.find("FILE=car_lod_b_edited.kn5") != std::string::npos &&
                exported.text.find("IN=16") != std::string::npos &&
                exported.text.find("OUT=50") != std::string::npos,
            "car LOD edits and distance settings export");
}

void rejectsMissingInvalidAndUnsafeEditsAtomically() {
    const auto captured = apex::authoring::captureProjectTrackWorkspaceBaseline(
        "models.ini", trackBytes());
    require(captured.ok() && captured.baseline.has_value(),
            "track baseline opens for rejection tests");

    ProjectState missing;
    WorkspaceFileEdit missingEdit;
    missingEdit.position = std::array<float, 3>{1, 2, 3};
    missing.workspaceFiles.emplace(2U, missingEdit);
    const auto missingResult = apex::authoring::applyProjectWorkspaceEdits(
        missing, *captured.baseline);
    require(!missingResult.ok() && !missingResult.candidate.has_value() &&
                missingResult.diagnostics.back().code == "MISSING_WORKSPACE_POSITION",
            "missing workspace position rejects without a candidate");

    ProjectState nonfinite;
    WorkspaceFileEdit nanEdit;
    nanEdit.position = std::array<float, 3>{
        std::numeric_limits<float>::quiet_NaN(), 0, 0};
    nonfinite.workspaceFiles.emplace(0U, nanEdit);
    const auto nonfiniteResult = apex::authoring::applyProjectWorkspaceEdits(
        nonfinite, *captured.baseline);
    require(!nonfiniteResult.ok() && !nonfiniteResult.candidate.has_value() &&
                nonfiniteResult.diagnostics.back().code == "INVALID_EDIT",
            "non-finite workspace edit rejects atomically");

    ProjectState unsafe;
    WorkspaceFileEdit unsafeEdit;
    unsafeEdit.name = "../escape.kn5";
    unsafe.workspaceFiles.emplace(0U, unsafeEdit);
    const auto unsafeResult = apex::authoring::applyProjectWorkspaceEdits(
        unsafe, *captured.baseline);
    require(!unsafeResult.ok() && !unsafeResult.candidate.has_value() &&
                unsafeResult.diagnostics.back().code == "INVALID_EDIT",
            "unsafe workspace edit rejects atomically");
}

void reportsFieldsThatTheManifestCannotStore() {
    const auto captured = apex::authoring::captureProjectTrackWorkspaceBaseline(
        "models.ini", trackBytes());
    require(captured.ok() && captured.baseline.has_value(),
            "track baseline opens for unsupported-field diagnostics");
    ProjectState project;
    project.workspace.cockpitHrDistance = 20.0F;
    WorkspaceFileEdit edit;
    edit.name = "ignored-on-track.kn5";
    edit.position = std::array<float, 3>{2, 3, 4};
    project.workspaceFiles.emplace(0U, edit);
    const auto exported = apex::authoring::exportProjectWorkspace(
        project, *captured.baseline);
    require(exported.ok() && exported.applied == 1U &&
                exported.text.find("POSITION=2, 3, 4") != std::string::npos &&
                exported.text.find("ignored-on-track.kn5") == std::string::npos &&
                std::count_if(exported.diagnostics.begin(), exported.diagnostics.end(),
                              [](const auto& item) {
                                  return item.code == "UNSUPPORTED_FIELD";
                              }) == 2,
            "workspace output reports settings and fields that it cannot store");
}

void rejectsMalformedAndOutputLimitedBaselines() {
    const std::string malformedText = "[MODEL_0]\nFILE=main.kn5\nPOSITION=1,2,\\";
    const std::vector<std::uint8_t> malformed(malformedText.begin(), malformedText.end());
    const auto malformedResult = apex::authoring::captureProjectTrackWorkspaceBaseline(
        "models.ini", malformed);
    require(!malformedResult.ok() && !malformedResult.baseline.has_value(),
            "truncated track manifest has no partial baseline");

    const auto captured = apex::authoring::captureProjectTrackWorkspaceBaseline(
        "models.ini", trackBytes());
    require(captured.ok() && captured.baseline.has_value(),
            "track baseline opens for output limit");
    WorkspaceLimits limits;
    limits.maxOutputBytes = 24U;
    const auto limited = apex::authoring::exportProjectWorkspace(
        ProjectState{}, *captured.baseline, limits);
    require(!limited.ok() && !limited.candidate.has_value() && limited.text.empty() &&
                limited.diagnostics.back().code == "OUTPUT_LIMIT",
            "workspace output limit rejects without partial output");

    const std::string malformedCarText = "[LOD_0]\nFILE=car.kn5\nIN=0\nOUT=\\";
    const std::vector<std::uint8_t> malformedCar(
        malformedCarText.begin(), malformedCarText.end());
    const auto malformedCarResult =
        apex::authoring::captureProjectCarLodWorkspaceBaseline(
            "data/lods.ini", malformedCar);
    require(!malformedCarResult.ok() && !malformedCarResult.baseline.has_value(),
            "truncated car LOD manifest has no partial baseline");
}

}  // namespace

int main() {
    try {
        capturesSparseTrackBaseline();
        appliesTrackStaticAndDynamicEditsDeterministically();
        appliesCarLodEditsAndSettings();
        rejectsMissingInvalidAndUnsafeEditsAtomically();
        reportsFieldsThatTheManifestCannotStore();
        rejectsMalformedAndOutputLimitedBaselines();
        std::cout << "project_workspace_tests: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "project_workspace_tests: " << error.what() << '\n';
        return 1;
    }
}
