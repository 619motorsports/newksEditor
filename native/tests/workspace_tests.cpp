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
using apex::workspace::serializeCarLodsIni;
using apex::workspace::serializeModelsIni;

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
    // KN5's serialized integer is the shader bind point, not a texture-table index.
    materialValue.resources.push_back(Kn5MaterialResource{"txDiffuse", 21, output.textures[0].name});
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

void matchesJavaScriptNumberManifestValues() {
    const std::string nbsp = "\xc2\xa0";
    const std::string ogham = "\xe1\x9a\x80";
    const std::string bom = "\xef\xbb\xbf";
    const auto parsed = parseModelsIni(
        "[DYNAMIC_OBJECT_0]\nFILE=shared.kn5\nPROBABILITY=" + nbsp + "0x10" + nbsp +
        "\nMULT=" + ogham + "0b10" + ogham + "," + bom + "0o3" + bom +
        "\nRND_POS_CENTER=" + nbsp + "1" + nbsp + ",2,3\n");
    require(parsed.dynamicObjects.size() == 1U &&
                parsed.dynamicObjects[0].probability == 16.0F &&
                parsed.dynamicObjects[0].multiplicity == std::array<float, 2>{2.0F, 3.0F} &&
                parsed.dynamicObjects[0].positionCenter == apex::workspace::Vector3{1.0F, 2.0F, 3.0F} &&
                parsed.warnings.empty(),
            "manifest values follow JavaScript Number radix and Unicode whitespace conversion");

    const auto malformed = parseModelsIni(
        "[DYNAMIC_OBJECT_0]\nFILE=shared.kn5\nPROBABILITY=0x\nMULT=0b2,0o8\n"
        "RND_POS_CENTER=1e9999,2,3\n");
    require(malformed.dynamicObjects.size() == 1U &&
                malformed.dynamicObjects[0].probability == 100.0F &&
                malformed.dynamicObjects[0].multiplicity == std::array<float, 2>{1.0F, 1.0F} &&
                malformed.dynamicObjects[0].positionCenter == apex::workspace::Vector3{} &&
                malformed.warnings.size() >= 3U,
            "manifest rejects prefix-only radix, invalid radix digits, and overflow values");
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
    require(merged.model.materials[0].resources[0].textureId == 21 &&
                merged.model.materials[1].resources[0].textureId == 21,
            "track texture dedupe preserves shader bind points");
    require(merged.model.bytesRead == 200 && merged.model.byteLength == 200, "aggregate byte metadata");

    WorkspaceOptions carOptions;
    carOptions.kind = "carLods";
    const auto car = mergeKn5Models(entries, carOptions);
    require(car.model.textures.size() == 2 && car.workspace.scopeResources, "car texture scoping");
    require(car.model.textures[0].workspaceFileIndex == 0U &&
                car.model.textures[1].workspaceFileIndex == 1U &&
                car.model.materials[0].workspaceFileIndex == 0U &&
                car.model.materials[1].workspaceFileIndex == 1U,
            "car resources retain synthetic workspace scope");
    require(car.model.materials[0].resources[0].textureId == 21 &&
                car.model.materials[1].resources[0].textureId == 21,
            "car texture scoping preserves shader bind points");
    require(car.workspace.materialRecords.size() == 2 && car.workspace.textureRecords.size() == 2,
            "scoped resource metadata");
}

void normalizesEmptyWorkspaceOptions() {
    const auto source = model("empty-options.kn5", "body", "body.dds");
    const std::array<WorkspaceModelInput, 1> entries = {input("empty-options.kn5", &source)};
    WorkspaceOptions options;
    options.name.clear();
    options.kind.clear();
    const auto merged = mergeKn5Models(entries, options);
    require(merged.model.root.name == "KN5 workspace" &&
                merged.workspace.name == "KN5 workspace" && merged.workspace.kind == "track" &&
                !merged.workspace.scopeResources,
            "empty workspace option name and kind use JavaScript fallbacks");
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

void serializesTrackManifestWithSparseAndDynamicEntries() {
    apex::workspace::WorkspaceMetadata workspace;
    apex::workspace::WorkspaceFile staticFile;
    staticFile.name = "main.kn5";
    staticFile.manifestIndex = 4U;
    staticFile.position = {1.2345678F, 0.0F, -0.0000001F};
    staticFile.rotation = {0.0F, 90.0F, 0.0F};

    apex::workspace::WorkspaceFile fallbackFile;
    fallbackFile.name = "details.kn5";
    fallbackFile.position = {2.0F, 3.0F, 4.0F};

    apex::workspace::WorkspaceFile dynamicFile;
    dynamicFile.name = "fly by.kn5";
    apex::workspace::DynamicObjectManifest dynamic;
    dynamic.index.reset();
    dynamic.probability = 75.0F;
    dynamic.multiplicity = {1.0F, 3.0F};
    dynamic.posMode = " fixed ";
    dynamic.positionCenter = {-10.0F, 20.0F, 30.0F};
    dynamic.positionRange = {4.0F, 5.0F, 6.0F};
    dynamic.velMode = "linear";
    dynamic.velocityBase = {1.0F, 2.0F, 3.0F};
    dynamic.velocityRange = {7.0F, 8.0F, 9.0F};
    dynamic.playWav = "fly by.wav";
    dynamicFile.dynamic = dynamic;
    dynamicFile.position = dynamic.positionCenter;

    apex::workspace::WorkspaceFile auxiliary;
    auxiliary.name = "driver.kn5";
    auxiliary.auxiliary = "driver";
    auxiliary.manifestIndex = 0U;
    workspace.files = {staticFile, fallbackFile, dynamicFile, auxiliary};

    const auto text = serializeModelsIni(workspace);
    require(text.find("[MODEL_4]\nFILE=main.kn5\nPOSITION=1.234568, 0, 0\nROTATION=0, 90, 0") !=
                std::string::npos,
            "sparse static manifest index and JavaScript number formatting");
    require(text.find("[MODEL_1]") != std::string::npos,
            "auxiliary explicit index reserves the independent static fallback index");
    require(text.find("[DYNAMIC_OBJECT_0]\nFILE='fly by.kn5'") != std::string::npos,
            "dynamic fallback index and quoted file");
    require(text.find("POS_MODE=FIXED") != std::string::npos &&
                text.find("VEL_MODE=LINEAR") != std::string::npos &&
                text.find("PLAY_WAV='fly by.wav'") != std::string::npos,
            "dynamic fields and mode normalization");
    const auto parsed = parseModelsIni(text);
    require(parsed.models.size() == 2U && parsed.models[0].index == 1U &&
                parsed.models[1].index == 4U,
            "serialized sparse static entries round-trip");
    require(parsed.dynamicObjects.size() == 1U && parsed.dynamicObjects[0].index == 0U &&
                parsed.dynamicObjects[0].positionRange == apex::workspace::Vector3{4, 5, 6} &&
                parsed.dynamicObjects[0].playWav == "fly by.wav" && parsed.warnings.empty(),
            "serialized dynamic fields round-trip without warnings");
}

void serializesCarLodManifestWithSwitchesAndPortableParentPath() {
    apex::workspace::WorkspaceMetadata workspace;
    workspace.manifest = "data/lods.ini";
    workspace.cockpitHrDistance = 6.0F;
    workspace.driverHrDistance = 25.125F;
    apex::workspace::WorkspaceFile first;
    first.name = "Car LOD A.kn5";
    first.lod = apex::workspace::CarLodManifest{0U, first.name, 0.0F, 15.0F, {}, 0U};
    apex::workspace::WorkspaceFile second;
    second.name = "body_lod_b.kn5";
    second.lod = apex::workspace::CarLodManifest{1U, second.name, 15.0F, 45.0F, {}, 0U};
    apex::workspace::WorkspaceFile driver;
    driver.name = "Driver.kn5";
    driver.auxiliary = "driver";
    workspace.files = {second, driver, first};

    const auto text = serializeCarLodsIni(workspace);
    require(text.find("[COCKPIT_HR]\nDISTANCE_SWITCH=6") != std::string::npos &&
                text.find("[DRIVER_HR]\nDISTANCE_SWITCH=25.125") != std::string::npos,
            "car distance switches formatting");
    require(text.find("FILE='Car LOD A.kn5'") != std::string::npos &&
                text.find("FILE=body_lod_b.kn5") != std::string::npos,
            "car LOD quoting and portable path");
    const auto parsed = parseCarLodsIni(text);
    require(parsed.lods.size() == 2U && parsed.lods[0].file == "Car LOD A.kn5" &&
                parsed.lods[1].file == "body_lod_b.kn5" &&
                parsed.cockpitHrDistance == 6.0F && parsed.driverHrDistance == 25.125F &&
                parsed.warnings.empty(),
            "car LOD manifest round-trip");
    second.name = "..\\shared_car\\body_lod_b.kn5";
    workspace.files[0].name = second.name;
    const auto parentRelative = serializeCarLodsIni(workspace);
    require(parentRelative.find("FILE=../shared_car/body_lod_b.kn5") != std::string::npos,
            "car LOD parent-relative path normalization");
}

void rejectsUnsafeAmbiguousAndUnboundedManifestOutput() {
    apex::workspace::WorkspaceMetadata workspace;
    apex::workspace::WorkspaceFile first;
    first.name = "first.kn5";
    first.manifestIndex = 2U;
    apex::workspace::WorkspaceFile second;
    second.name = "second.kn5";
    second.manifestIndex = 2U;
    workspace.files = {first, second};
    expectsError([&] { (void)serializeModelsIni(workspace); }, "AMBIGUOUS_REFERENCE");

    workspace.files[1].manifestIndex.reset();
    workspace.files[1].name = "../escape.kn5";
    expectsError([&] { (void)serializeModelsIni(workspace); }, "UNSAFE_REFERENCE");

    workspace.files[1].name = "second.kn5";
    workspace.files[1].position[0] = std::numeric_limits<float>::infinity();
    expectsError([&] { (void)serializeModelsIni(workspace); }, "NON_FINITE_VALUE");

    workspace.files = {first};
    WorkspaceLimits outputLimit;
    outputLimit.maxOutputBytes = 8U;
    expectsError([&] { (void)serializeModelsIni(workspace, outputLimit); }, "OUTPUT_LIMIT");

    apex::workspace::WorkspaceMetadata car;
    apex::workspace::WorkspaceFile carFirst;
    carFirst.name = "car.kn5";
    carFirst.lod = apex::workspace::CarLodManifest{0U, carFirst.name, 0.0F, 10.0F, {}, 0U};
    apex::workspace::WorkspaceFile carSecond = carFirst;
    carSecond.name = "car_lod_b.kn5";
    carSecond.lod->index = 0U;
    car.files = {carFirst, carSecond};
    expectsError([&] { (void)serializeCarLodsIni(car); }, "AMBIGUOUS_REFERENCE");

    car.files[1].lod->index = 1U;
    car.files[1].name = "car?.kn5";
    expectsError([&] { (void)serializeCarLodsIni(car); }, "UNSAFE_REFERENCE");
}

} // namespace

int main() {
    try {
        parsesTrackAndContiguousDynamicObjects();
        matchesJavaScriptNumberManifestValues();
        parsesCarLodsAndHalfOpenRanges();
        mergesScenesWithTransformsAndResourceRemapping();
        normalizesEmptyWorkspaceOptions();
        assemblesManifestInputsAndRejectsInvalidReferences();
        reusesManifestInputsAndAppliesDynamicCenters();
        serializesTrackManifestWithSparseAndDynamicEntries();
        serializesCarLodManifestWithSwitchesAndPortableParentPath();
        rejectsUnsafeAmbiguousAndUnboundedManifestOutput();
        std::cout << "workspace tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "workspace tests failed: " << error.what() << '\n';
        return 1;
    }
}
