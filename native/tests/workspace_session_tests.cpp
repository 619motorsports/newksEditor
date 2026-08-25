#include "apex/app/workspace_session.hpp"
#include "apex/formats/kn5_write.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::app::WorkspaceSession;
using apex::app::WorkspaceSessionFile;
using apex::app::WorkspaceSessionKind;
using apex::app::WorkspaceSessionLimits;
using apex::app::WorkspaceSessionOpenRequest;
using apex::formats::Kn5File;
using apex::formats::Kn5Material;
using apex::formats::Kn5Node;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Kn5File model(std::string name) {
    Kn5File output;
    output.source = std::move(name);
    output.version = 6;
    output.root.type = 1;
    output.root.kind = "node";
    output.root.name = "root";
    output.root.active = true;
    output.root.transform = {1, 0, 0, 0, 0, 1, 0, 0,
                             0, 0, 1, 0, 0, 0, 0, 1};
    Kn5Material material;
    material.name = "material";
    material.shader = "ksPerPixel";
    output.materials.push_back(std::move(material));
    Kn5Node mesh;
    mesh.type = 2;
    mesh.kind = "mesh";
    mesh.name = "mesh";
    mesh.active = true;
    mesh.visible = true;
    mesh.renderable = true;
    mesh.castShadows = true;
    mesh.materialId = 0;
    mesh.vertices.resize(22U, 0.0F);
    mesh.indices = {0U, 1U};
    mesh.bounds = {0, 0, 0, 1};
    output.root.children.push_back(std::move(mesh));
    return output;
}

std::vector<std::uint8_t> modelBytes(std::string name) {
    return apex::formats::serializeKn5(model(name));
}

WorkspaceSessionOpenRequest trackRequest(
    std::string manifest, std::vector<WorkspaceSessionFile>& files) {
    WorkspaceSessionOpenRequest request;
    request.kind = WorkspaceSessionKind::track;
    request.name = "track workspace";
    request.manifestName = "data/models.ini";
    request.manifestBytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size());
    request.modelFiles = files;
    return request;
}

void opensTrackAndCarWorkspaces() {
    const auto first = modelBytes("main.kn5");
    const auto second = modelBytes("detail.kn5");
    const std::string trackManifest =
        "[MODEL_0]\nFILE=main.kn5\nPOSITION=1,2,3\n"
        "[MODEL_1]\nFILE=detail.kn5\nPOSITION=4,5,6\n";
    std::vector<WorkspaceSessionFile> trackFiles = {
        {"main.kn5", first}, {"detail.kn5", second}};
    const auto track = WorkspaceSession().open(trackRequest(trackManifest, trackFiles));
    require(track.ok(), "track workspace opens");
    require(track.document->assembly.workspace.kind == "track" &&
                track.document->assembly.workspace.files.size() == 2U,
            "track metadata is assembled");
    require(track.document->scene.snapshot.workspace_kind == "track" &&
                track.document->sceneBinding.file_root_nodes.size() == 2U,
            "track scene is bound");
    require(track.document->assembly.model.root.children[1].transform[12] == 4.0F,
            "track placement is retained");

    const std::string carManifest =
        "[LOD_0]\nFILE=main.kn5\nIN=0\nOUT=20\n"
        "[LOD_1]\nFILE=detail.kn5\nIN=20\nOUT=100\n";
    WorkspaceSessionOpenRequest carRequest;
    carRequest.kind = WorkspaceSessionKind::carLods;
    carRequest.name = "car workspace";
    carRequest.manifestName = "data/lods.ini";
    carRequest.manifestBytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(carManifest.data()), carManifest.size());
    carRequest.modelFiles = trackFiles;
    const auto car = WorkspaceSession().open(carRequest);
    require(car.ok() && car.document->assembly.workspace.kind == "carLods" &&
                car.document->assembly.workspace.files[1].lod.has_value(),
            "car LOD workspace opens");

    WorkspaceSessionOpenRequest genericRequest;
    genericRequest.name = "generic workspace";
    genericRequest.modelFiles = trackFiles;
    const auto generic = WorkspaceSession().open(genericRequest);
    require(generic.ok() && generic.document->assembly.workspace.kind == "generic" &&
                generic.document->assembly.workspace.files.size() == 2U,
            "generic multi-model workspace opens without a manifest");
}

void rejectsReferencesTruncationAndBudgets() {
    const auto bytes = modelBytes("main.kn5");
    const std::string manifest = "[MODEL_0]\nFILE=main.kn5\n";

    std::vector<WorkspaceSessionFile> validFiles = {{"main.kn5", bytes}};
    auto missingManifest = trackRequest(manifest, validFiles);
    missingManifest.manifestBytes = {};
    const auto missingManifestResult = WorkspaceSession().open(missingManifest);
    require(!missingManifestResult.ok() &&
                missingManifestResult.diagnostics.front().code == "INVALID_MANIFEST",
            "track workspaces require manifest bytes");

    auto genericWithManifest = trackRequest(manifest, validFiles);
    genericWithManifest.kind = WorkspaceSessionKind::generic;
    const auto genericManifestResult = WorkspaceSession().open(genericWithManifest);
    require(!genericManifestResult.ok() &&
                genericManifestResult.diagnostics.front().code == "INVALID_KIND",
            "generic workspaces reject manifest input");

    std::vector<WorkspaceSessionFile> unsafeFiles = {{"../main.kn5", bytes}};
    const auto unsafe = WorkspaceSession().open(trackRequest(manifest, unsafeFiles));
    require(!unsafe.ok() && unsafe.diagnostics.front().code == "UNSAFE_REFERENCE",
            "caller-granted model names cannot traverse the workspace root");

    std::vector<WorkspaceSessionFile> missingFiles = {
        {"other.kn5", bytes}};
    const auto missing = WorkspaceSession().open(trackRequest(manifest, missingFiles));
    require(!missing.ok() && !missing.diagnostics.empty() &&
                missing.diagnostics.front().code == "INVALID_REFERENCE" &&
                !missing.document.has_value(),
            "missing model reference fails atomically");

    std::vector<WorkspaceSessionFile> duplicateFiles = {
        {"main.kn5", bytes}, {"main.kn5", bytes}};
    const auto duplicate = WorkspaceSession().open(trackRequest(manifest, duplicateFiles));
    require(!duplicate.ok() && duplicate.diagnostics.front().code == "AMBIGUOUS_REFERENCE" &&
                !duplicate.document.has_value(),
            "ambiguous model reference fails atomically");

    const std::vector<std::uint8_t> truncated(bytes.begin(), bytes.end() - 1);
    std::vector<WorkspaceSessionFile> truncatedFiles = {{"main.kn5", truncated}};
    const auto truncatedResult = WorkspaceSession().open(trackRequest(manifest, truncatedFiles));
    require(!truncatedResult.ok() && !truncatedResult.diagnostics.empty() &&
                truncatedResult.diagnostics.front().code == "MODEL_INVALID" &&
                !truncatedResult.document.has_value(),
            std::string("truncated model fails atomically: ") +
                (truncatedResult.diagnostics.empty() ? "no diagnostic"
                                                      : truncatedResult.diagnostics.front().code));

    WorkspaceSessionLimits inputLimits;
    inputLimits.maxTotalInputBytes = manifest.size() + bytes.size() - 1U;
    const auto inputLimited = WorkspaceSession(inputLimits).open(trackRequest(manifest, validFiles));
    require(!inputLimited.ok() && inputLimited.diagnostics.front().code == "INPUT_LIMIT",
            "aggregate input budget is enforced before parsing");

    WorkspaceSessionLimits workspaceLimits;
    workspaceLimits.workspace.maxFiles = 0U;
    const auto workspaceLimited = WorkspaceSession(workspaceLimits).open(
        trackRequest(manifest, validFiles));
    require(!workspaceLimited.ok() && workspaceLimited.diagnostics.front().code == "COUNT_LIMIT" &&
                !workspaceLimited.document.has_value(),
            "workspace assembly budget is enforced atomically");
}

std::filesystem::path temporaryRoot() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("apex-workspace-session-" + std::to_string(static_cast<long long>(stamp)));
}

void writeFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot create workspace source fixture");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void opensAssetSourceWorkspace() {
    const auto root = temporaryRoot();
    std::filesystem::create_directories(root / "data");
    const auto bytes = modelBytes("main.kn5");
    writeFile(root / "data/main.kn5", bytes);
    const std::string manifest = "[MODEL_0]\nFILE=main.kn5\n";
    {
        std::ofstream output(root / "data/models.ini", std::ios::binary);
        output << manifest;
    }

    try {
        apex::assets::AssetSource source;
        source.addDirectory(root, "track");
        const auto result = WorkspaceSession().openAssetSource(
            WorkspaceSessionKind::track, "source track", "data/models.ini", source);
        require(result.ok() && result.document->assembly.workspace.files.size() == 1U,
                "asset source workspace opens");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    const auto packedModel = modelBytes("packed.kn5");
    const std::string packedManifest = "[MODEL_0]\nFILE=packed.kn5\n";
    apex::formats::AcdArchive archive;
    apex::formats::AcdEntry manifestEntry;
    manifestEntry.name = "models.ini";
    manifestEntry.path = "models.ini";
    manifestEntry.safe = true;
    manifestEntry.data.assign(packedManifest.begin(), packedManifest.end());
    manifestEntry.size = manifestEntry.data.size();
    apex::formats::AcdEntry modelEntry;
    modelEntry.name = "packed.kn5";
    modelEntry.path = "packed.kn5";
    modelEntry.safe = true;
    modelEntry.data = packedModel;
    modelEntry.size = modelEntry.data.size();
    archive.entries = {std::move(manifestEntry), std::move(modelEntry)};
    apex::assets::AssetSource packedSource;
    packedSource.addAcdArchive(archive);
    const auto packed = WorkspaceSession().openAssetSource(
        WorkspaceSessionKind::track, "packed track", "models.ini", packedSource);
    require(packed.ok() && packed.document->assembly.workspace.files.size() == 1U,
            "packed asset source workspace opens");
}

}  // namespace

int main() {
    try {
        opensTrackAndCarWorkspaces();
        rejectsReferencesTruncationAndBudgets();
        opensAssetSourceWorkspace();
        std::cout << "workspace_session_tests: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "workspace_session_tests: " << error.what() << '\n';
        return 1;
    }
}
