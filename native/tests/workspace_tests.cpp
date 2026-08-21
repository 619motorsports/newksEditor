#include "apex/workspace/workspace.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using apex::core::ParseError;
using apex::formats::Kn5File;
using apex::formats::Kn5Material;
using apex::formats::Kn5MaterialResource;
using apex::formats::Kn5Node;
using apex::formats::Kn5Texture;
using apex::workspace::CarManifest;
using apex::workspace::WorkspaceAssembly;
using apex::workspace::WorkspaceLimits;
using apex::workspace::WorkspaceModelInput;
using apex::workspace::WorkspaceOptions;
using apex::workspace::assembleCarLodWorkspace;
using apex::workspace::assembleTrackWorkspace;
using apex::workspace::carLodDistance;
using apex::workspace::carLodVisible;
using apex::workspace::mergeKn5Models;
using apex::workspace::modelPlacementMatrix;
using apex::workspace::parseCarLodsIni;
using apex::workspace::parseModelsIni;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

WorkspaceModelInput input(std::string name, const Kn5File* model, std::size_t size = 0,
                          apex::workspace::Vector3 position = {},
                          apex::workspace::Vector3 rotation = {}) {
    WorkspaceModelInput output;
    output.name = std::move(name);
    output.model = model;
    output.size = size;
    output.position = position;
    output.rotation = rotation;
    return output;
}

Kn5File model(std::string source, std::string materialName, std::string textureName,
              std::uint32_t materialId = 0) {
    Kn5File output;
    output.source = std::move(source);
    output.version = 6;
    output.sourceMarker = 1;
    output.bytesRead = 100;
    output.byteLength = 100;
    Kn5Texture texture;
    texture.active = true;
    texture.name = std::move(textureName);
    texture.size = 8;
    output.textures.push_back(std::move(texture));
    Kn5Material materialValue;
    materialValue.name = std::move(materialName);
    materialValue.shader = "ksPerPixel";
    materialValue.resources.push_back(Kn5MaterialResource{"txDiffuse", 0, output.textures[0].name});
    output.materials.push_back(std::move(materialValue));
    output.root.type = 1;
    output.root.kind = "node";
    output.root.name = "root";
    output.root.active = true;
    output.root.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    Kn5Node mesh;
    mesh.type = 2;
    mesh.kind = "mesh";
    mesh.name = "mesh";
    mesh.active = true;
    mesh.visible = true;
    mesh.renderable = true;
    mesh.materialId = materialId;
    output.root.children.push_back(std::move(mesh));
    return output;
}

template <typename Function>
void expectsError(Function&& function, std::string_view code) {
    try {
        function();
    } catch (const ParseError& error) {
        require(error.format() == "WORKSPACE", "workspace error attribution");
        require(error.code() == code, "unexpected workspace error code");
        return;
    }
    throw std::runtime_error("workspace accepted malformed input");
}

void parsesTrackAndContiguousDynamicObjects() {
    const auto parsed = parseModelsIni(
        "[MODEL_2]\nFILE=details.kn5\nPOSITION=1,2,3\nROTATION=90,0,0\n"
        "[DYNAMIC_OBJECT_0]\nFILE=plane.kn5\nPROBABILITY=75\nMULT=1,3\n"
        "RND_POS_CENTER=100,200,300\nRND_VEL_RANGE=4,5,6\n"
        "[MODEL_0]\nFILE=main.kn5\n[DYNAMIC_OBJECT_2]\nFILE=ignored.kn5\n");
    require(parsed.models.size() == 2 && parsed.models[0].index == 0 && parsed.models[1].index == 2,
            "static model ordering");
    require(parsed.models[1].position == apex::workspace::Vector3{1, 2, 3}, "model position");
    require(parsed.dynamicObjects.size() == 2 && parsed.dynamicObjects[0].probability == 75.0F,
            "dynamic parsing");
    require(parsed.dynamicObjects[0].multiplicity == std::array<float, 2>{1, 3}, "dynamic multiplicity");
    std::vector<std::string> warnings;
    const auto contiguous = apex::workspace::contiguousDynamicTrackObjects(parsed.dynamicObjects, warnings);
    require(contiguous.size() == 1 && contiguous[0].index == 0, "contiguous dynamic prefix");
    require(!warnings.empty(), "gapped dynamic diagnostic");

    const auto unsafe = parseModelsIni("[MODEL_0]\nFILE=../escape.kn5\n[MODEL_1]\nFILE=C:\\\\x.kn5\n");
    require(unsafe.models.empty() && unsafe.warnings.size() >= 2, "unsafe manifest references rejected");
}

void parsesCarLodsAndHalfOpenRanges() {
    const auto parsed = parseCarLodsIni(
        "[COCKPIT_HR]\nDISTANCE_SWITCH=25\n[DRIVER_HR]\nDISTANCE_SWITCH=25\n"
        "[LOD_0]\nFILE=car.kn5\nIN=0\nOUT=15\n"
        "[LOD_1]\nFILE=car_lod_b.kn5\nIN=15\nOUT=45\n"
        "[LOD_3]\nFILE=ignored.kn5\nIN=200\nOUT=5000\n");
    require(parsed.lods.size() == 2, "contiguous LOD prefix");
    require(parsed.cockpitHrDistance == 25.0F && parsed.driverHrDistance == 25.0F,
            "distance switches");
    require(!parsed.warnings.empty(), "gapped LOD diagnostic");
    require(carLodVisible(&parsed.lods[0], 14.999F), "LOD lower range");
    require(!carLodVisible(&parsed.lods[0], 15.0F), "LOD upper boundary is exclusive");
    require(carLodVisible(&parsed.lods[0], 1.0F, 0), "selected LOD");
    require(!carLodVisible(&parsed.lods[0], 1.0F, 1), "different selected LOD");
    require(std::abs(carLodDistance(20, 45) - 15) < 1e-5F, "LOD distance");

    const auto fourLods = parseCarLodsIni(
        "[LOD_0]\nFILE=lod0.kn5\nIN=0\nOUT=10\n"
        "[LOD_1]\nFILE=lod1.kn5\nIN=10\nOUT=25\n"
        "[LOD_2]\nFILE=lod2.kn5\nIN=25\nOUT=60\n"
        "[LOD_3]\nFILE=lod3.kn5\nIN=60\nOUT=1000\n");
    require(fourLods.lods.size() == 4 && fourLods.lods[3].index == 3,
            "four contiguous car LODs");

    const auto malformed = parseCarLodsIni("[LOD_0]\nFILE=../shared/car.kn5\nIN=0\nOUT=5\n");
    require(malformed.lods.size() == 1 && malformed.lods[0].file.empty() && !malformed.warnings.empty(),
            "unsafe car LOD reference diagnostic");
}

void mergesScenesWithTransformsAndResourceRemapping() {
    const auto first = model("first.kn5", "road", "shared.dds");
    const auto second = model("second.kn5", "grass", "shared.dds");
    const std::array<WorkspaceModelInput, 2> entries = {
        input("first.kn5", &first, 100), input("second.kn5", &second, 100, {4, 5, 6}, {90, 0, 0})};
    WorkspaceOptions options;
    options.name = "layout";
    options.manifest = "models.ini";
    const auto merged = mergeKn5Models(entries, options);
    require(merged.model.root.children.size() == 2, "workspace wrapper nodes");
    require(merged.model.materials.size() == 2, "material append");
    require(merged.model.textures.size() == 1, "track texture dedupe");
    require(merged.model.root.children[1].children[0].children[0].materialId == 1,
            "material ID remap");
    require(merged.model.root.children[1].transform[12] == 4.0F, "placement translation");
    require(merged.workspace.textureCollisions.size() == 1, "texture collision diagnostic");
    require(merged.model.materials[1].resources[0].textureId == 0,
            "deduplicated texture ID remap");
    require(merged.model.bytesRead == 200 && merged.model.byteLength == 200, "aggregate byte metadata");

    WorkspaceOptions carOptions;
    carOptions.kind = "carLods";
    const auto car = mergeKn5Models(entries, carOptions);
    require(car.model.textures.size() == 2 && car.workspace.scopeResources, "car texture scoping");
    require(car.model.materials[1].resources[0].textureId == 1, "scoped texture ID remap");
    require(car.workspace.materialRecords.size() == 2 && car.workspace.textureRecords.size() == 2,
            "scoped resource metadata");
}

void assemblesManifestInputsAndRejectsInvalidReferences() {
    const auto first = model("first.kn5", "road", "road.dds");
    const auto second = model("second.kn5", "grass", "grass.dds");
    const std::array<WorkspaceModelInput, 2> available = {
        input("first.kn5", &first), input("second.kn5", &second)};
    const auto manifest = parseModelsIni("[MODEL_0]\nFILE=first.kn5\nPOSITION=1,2,3\n[MODEL_1]\nFILE=second.kn5\n");
    const auto assembled = assembleTrackWorkspace(manifest, available);
    require(assembled.workspace.files.size() == 2 && assembled.workspace.files[0].manifestIndex == 0U,
            "track manifest assembly");
    const auto carManifest = parseCarLodsIni("[LOD_0]\nFILE=first.kn5\nIN=0\nOUT=15\n[LOD_1]\nFILE=second.kn5\nIN=15\nOUT=45\n");
    const auto car = assembleCarLodWorkspace(carManifest, available);
    require(car.workspace.files.size() == 2 && car.workspace.files[1].lod->inDistance == 15.0F,
            "car manifest assembly");
    const auto missing = parseModelsIni("[MODEL_0]\nFILE=missing.kn5\n");
    expectsError([&] { (void)assembleTrackWorkspace(missing, available); }, "INVALID_REFERENCE");
    const auto malformedCar = parseCarLodsIni("[LOD_0]\nFILE=../escape.kn5\nIN=0\nOUT=5\n");
    expectsError([&] { (void)assembleCarLodWorkspace(malformedCar, available); }, "INVALID_REFERENCE");
    WorkspaceLimits limit;
    limit.maxNodes = 1;
    WorkspaceOptions limited;
    limited.limits = limit;
    expectsError([&] { (void)mergeKn5Models(std::span<const WorkspaceModelInput>(available), limited); }, "NODE_LIMIT");
    auto invalid = model("invalid.kn5", "bad", "bad.dds", 3);
    const std::array<WorkspaceModelInput, 1> invalidInput = {input("invalid.kn5", &invalid)};
    expectsError([&] { (void)mergeKn5Models(invalidInput); }, "INVALID_REFERENCE");

    WorkspaceLimits manifestLimit;
    manifestLimit.maxManifestEntries = 1;
    expectsError([&] {
        (void)parseModelsIni("[MODEL_0]\nFILE=first.kn5\n[MODEL_1]\nFILE=second.kn5\n",
                             "models.ini", {}, manifestLimit);
    }, "COUNT_LIMIT");

    auto protectedModel = model("protected.kn5", "protected", "protected.dds");
    apex::formats::Kn5EncryptionInspection inspection;
    inspection.valid = true;
    inspection.recordCount = 2;
    protectedModel.encryption = inspection;
    const std::array<WorkspaceModelInput, 1> protectedInput = {input("protected.kn5", &protectedModel)};
    const auto protectedWorkspace = mergeKn5Models(protectedInput);
    require(protectedWorkspace.workspace.files[0].protectedFile &&
                protectedWorkspace.workspace.protectedFiles.size() == 1,
            "protected file diagnostic");
}

void reusesManifestInputsAndAppliesDynamicCenters() {
    const auto source = model("shared.kn5", "shared", "shared.dds");
    const std::array<WorkspaceModelInput, 1> available = {input("shared.kn5", &source)};
    const auto repeated = parseModelsIni(
        "[MODEL_0]\nFILE=shared.kn5\nPOSITION=1,0,0\n"
        "[MODEL_1]\nFILE=shared.kn5\nPOSITION=2,0,0\n");
    const auto repeatedWorkspace = assembleTrackWorkspace(repeated, available);
    require(repeatedWorkspace.workspace.files.size() == 2 &&
                repeatedWorkspace.model.root.children.size() == 2,
            "repeated manifest references are reused");
    require(repeatedWorkspace.model.root.children[0].transform[12] == 1.0F &&
                repeatedWorkspace.model.root.children[1].transform[12] == 2.0F,
            "repeated manifest transforms");

    const auto dynamicManifest = parseModelsIni(
        "[DYNAMIC_OBJECT_0]\nFILE=shared.kn5\nRND_POS_CENTER=7,8,9\n");
    const auto dynamicWorkspace = assembleTrackWorkspace(dynamicManifest, available);
    require(dynamicWorkspace.workspace.files.size() == 1 &&
                dynamicWorkspace.model.root.children[0].transform[12] == 7.0F &&
                dynamicWorkspace.model.root.children[0].transform[13] == 8.0F &&
                dynamicWorkspace.model.root.children[0].transform[14] == 9.0F,
            "dynamic position center transform");

    auto malformed = input("shared.kn5", &source);
    malformed.position[0] = std::numeric_limits<float>::quiet_NaN();
    const std::array<WorkspaceModelInput, 1> malformedInput = {malformed};
    expectsError([&] { (void)mergeKn5Models(malformedInput); }, "NON_FINITE_TRANSFORM");
}

} // namespace

int main() {
    try {
        parsesTrackAndContiguousDynamicObjects();
        parsesCarLodsAndHalfOpenRanges();
        mergesScenesWithTransformsAndResourceRemapping();
        assemblesManifestInputsAndRejectsInvalidReferences();
        reusesManifestInputsAndAppliesDynamicCenters();
        std::cout << "workspace tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "workspace tests failed: " << error.what() << '\n';
        return 1;
    }
}
