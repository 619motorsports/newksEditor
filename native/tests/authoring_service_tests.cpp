#include "apex/app/authoring_service.hpp"
#include "apex/formats/kn5_write.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::app::AuthoringService;
using apex::app::AuthoringServiceStatus;
using apex::authoring::AuthoringTransaction;
using apex::authoring::SetColliderAsset;
using apex::authoring::SetColliderEdit;
using apex::authoring::SetDamageAsset;
using apex::authoring::SetDamageEdit;
using apex::authoring::SetBottomColliderAsset;
using apex::authoring::SetBottomColliderEdit;
using apex::authoring::SetMeshEdit;
using apex::authoring::SetMaterialScalarEdit;
using apex::authoring::SetNodeEdit;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

apex::formats::Kn5Node rootNode(std::uint32_t type = 1U) {
    apex::formats::Kn5Node node;
    node.type = type;
    node.kind = type == 1U ? "node" : "mesh";
    node.name = type == 1U ? "root" : "BODY";
    node.active = true;
    node.visible = type != 1U;
    node.renderable = type != 1U;
    node.castShadows = type != 1U;
    node.vertexStride = type == 1U ? 0U : 11U;
    node.transform = {1, 0, 0, 0, 0, 1, 0, 0,
                      0, 0, 1, 0, 0, 0, 0, 1};
    return node;
}

apex::formats::Kn5File primaryModel(std::string meshName = "BODY") {
    apex::formats::Kn5File model;
    model.version = 6U;
    apex::formats::Kn5Material material;
    material.name = "Body";
    material.shader = "ksPerPixel";
    model.materials.push_back(std::move(material));
    model.root = rootNode();
    auto mesh = rootNode(2U);
    mesh.name = std::move(meshName);
    mesh.materialId = 0U;
    mesh.vertices = {
        0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0,
        1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0,
        0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0};
    mesh.indices = {0, 1, 2};
    mesh.bounds = {0, 0, 0, 1};
    mesh.lodIn = 0.0F;
    mesh.lodOut = 1000.0F;
    model.root.children.push_back(std::move(mesh));
    return model;
}

std::vector<std::uint8_t> modelBytes(std::string meshName = "BODY") {
    return apex::formats::serializeKn5(primaryModel(std::move(meshName)));
}

std::vector<std::uint8_t> changedColliderBytes() {
    return modelBytes("COLLIDER_BODY");
}

void opensPrimaryAndKeepsStateAtomic() {
    AuthoringService service;
    const auto bytes = modelBytes();
    const auto opened = service.openPrimary("car.kn5", bytes);
    require(opened.ok() && opened.identity.has_value() && service.isOpen(),
            "primary KN5 opens with an identity");

    AuthoringTransaction transaction;
    transaction.label = "rename body";
    apex::authoring::NodeEdit nodeEdit;
    nodeEdit.name = "BODY_EDITED";
    transaction.operations = {SetNodeEdit{"0", nodeEdit}};
    require(service.commit(transaction).ok(), "primary transaction commits");
    const auto before = service.state()->nodes.at("0").name;
    const auto revision = service.state()->revision;

    const std::vector<std::uint8_t> truncated(bytes.begin(), bytes.begin() + 3U);
    const auto failed = service.openPrimary("broken.kn5", truncated);
    require(failed.status == AuthoringServiceStatus::invalid &&
                service.state()->nodes.at("0").name == before &&
                service.state()->revision == revision,
            "failed primary open leaves the previous state unchanged");
}

void projectRoundTripUndoRedoAndExports() {
    const auto bytes = modelBytes();
    AuthoringService service;
    require(service.openPrimary("car.kn5", bytes).ok(), "service opens for project round trip");

    AuthoringTransaction transaction;
    transaction.label = "author body";
    apex::authoring::MeshEdit mesh;
    mesh.castShadows = false;
    transaction.operations = {
        SetMeshEdit{"BODY", mesh},
        SetMaterialScalarEdit{"Body", "shader", std::string("ksPerPixel")},
        SetMaterialScalarEdit{"Body", "blendMode", std::string("ALPHA")}};
    require(service.commit(transaction).ok(), "geometry transaction commits");

    const auto firstCsp = service.exportCsp();
    const auto secondCsp = service.exportCsp();
    require(firstCsp.ok() && firstCsp.text == secondCsp.text,
            "CSP export is deterministic");

    AuthoringTransaction nodeTransaction;
    nodeTransaction.label = "author hierarchy";
    apex::authoring::NodeEdit nodeEdit;
    nodeEdit.active = false;
    nodeTransaction.operations = {SetNodeEdit{"0", nodeEdit}};
    require(service.commit(nodeTransaction).ok(), "node transaction commits");
    const auto saved = service.saveProject();
    require(saved.ok() && saved.text.find("apex-editor-project") != std::string::npos,
            "project saves as owned JSON text");

    const auto firstKn5 = service.exportPrimaryKn5();
    const auto secondKn5 = service.exportPrimaryKn5();
    require(firstKn5.ok() && firstKn5.bytes == secondKn5.bytes &&
                !firstKn5.bytes.empty(),
            "primary KN5 export is deterministic and owned");
    require(std::any_of(firstKn5.diagnostics.begin(), firstKn5.diagnostics.end(),
                        [](const auto& item) { return item.code == "BAKE_WARNING"; }),
            "primary export surfaces project and bake warnings");

    const auto authoredRevision = service.state()->revision;
    require(service.undo().ok() && !service.state()->nodes.contains("0"),
            "undo removes the latest node edit");
    require(service.redo().ok() && service.state()->nodes.at("0").active == false &&
                service.state()->revision > authoredRevision,
            "redo restores the authored state");

    AuthoringService loaded;
    require(loaded.openPrimary("car.kn5", bytes).ok(), "fresh service opens for load");
    require(loaded.loadProject(saved.text).ok() && loaded.state()->nodes.at("0").active == false,
            "saved project loads into a fresh session");
    require(loaded.state()->revision == 1U,
            "project recovery assigns the documented service-local revision");

    const auto beforeFailure = loaded.saveProject().text;
    const auto malformed = loaded.loadProject("{\"format\":\"apex-editor-project\"");
    require(malformed.status == AuthoringServiceStatus::invalid &&
                loaded.saveProject().text == beforeFailure,
            "truncated project is rejected without partial state");

    std::string stale = saved.text;
    const auto hash = stale.find("\"sha256\": \"");
    require(hash != std::string::npos, "saved project contains source identity");
    const auto first = hash + std::string("\"sha256\": \"").size();
    stale[first] = stale[first] == 'a' ? 'b' : 'a';
    const auto staleResult = loaded.loadProject(stale);
    require(staleResult.status == AuthoringServiceStatus::invalid &&
                loaded.saveProject().text == beforeFailure,
            "stale project identity is rejected atomically");
}

void secondaryAssetsBindAndFailClosed() {
    const auto primary = modelBytes();
    const auto collider = modelBytes();
    const auto changedCollider = changedColliderBytes();
    const std::vector<std::uint8_t> damage{
        '[', 'S', 'C', 'R', 'A', 'T', 'C', 'H', 'E', 'S', ']', '\n',
        'M', 'I', 'N', '_', 'S', 'P', 'E', 'E', 'D', '=', '1', '\n'};
    const std::vector<std::uint8_t> bottom{
        '[', 'C', 'O', 'L', 'L', 'I', 'D', 'E', 'R', '_', '2', ']', '\n',
        'C', 'E', 'N', 'T', 'R', 'E', '=', '0', ',', ' ', '0', ',', ' ', '0', '\n',
        'S', 'I', 'Z', 'E', '=', '1', ',', ' ', '2', ',', ' ', '3', '\n'};

    AuthoringService service;
    require(service.openPrimary("car.kn5", primary).ok(), "service opens for secondary assets");
    const auto colliderOpen = service.openCollider("collider.kn5", collider);
    const auto damageOpen = service.openDamage("data/damage.ini", damage);
    const auto bottomOpen = service.openBottomColliders("data/colliders.ini", bottom);
    require(colliderOpen.ok() && damageOpen.ok() && bottomOpen.ok(),
            "all secondary assets bind from caller-owned bytes");

    AuthoringTransaction bind;
    bind.label = "bind secondary assets";
    apex::authoring::ColliderEdit colliderEdit;
    colliderEdit.reverseWinding = true;
    apex::authoring::DamageEdit damageEdit;
    damageEdit.minSpeed = 5.0F;
    apex::authoring::BottomColliderEdit bottomEdit;
    bottomEdit.centre = apex::authoring::Vector3{4, 5, 6};
    bind.operations = {
        SetColliderAsset{*colliderOpen.identity}, SetColliderEdit{"0", colliderEdit},
        SetDamageAsset{*damageOpen.identity}, SetDamageEdit{"SCRATCHES", damageEdit},
        SetBottomColliderAsset{*bottomOpen.identity}, SetBottomColliderEdit{0, bottomEdit}};
    require(service.commit(bind).ok(), "secondary identity transaction commits");

    const auto colliderExport = service.exportColliderKn5();
    const auto damageExport = service.exportDamageIni();
    const auto bottomExport = service.exportBottomCollidersIni();
    require(colliderExport.ok() && !colliderExport.bytes.empty() &&
                damageExport.ok() && damageExport.text.find("MIN_SPEED=5") != std::string::npos &&
                bottomExport.ok() && bottomExport.text.find("CENTRE=4, 5, 6") != std::string::npos,
            "secondary assets export through the native adapters");
    require(service.exportColliderKn5().bytes == colliderExport.bytes &&
                service.exportDamageIni().text == damageExport.text &&
                service.exportBottomCollidersIni().text == bottomExport.text,
            "secondary exports are deterministic from immutable baselines");

    const std::vector<std::uint8_t> truncatedCollider(collider.begin(),
                                                       collider.begin() + 3U);
    require(service.openCollider("collider.kn5", truncatedCollider).status ==
                AuthoringServiceStatus::invalid &&
                service.exportColliderKn5().bytes == colliderExport.bytes,
            "truncated collider input cannot replace the bound baseline");
    const std::vector<std::uint8_t> malformedDamage{
        '[', 'S', 'C', 'R', 'A', 'T', 'C', 'H', 'E', 'S', ']', '\n',
        'M', 'I', 'N', '_', 'S', 'P', 'E', 'E', 'D', '=', '1', '\\'};
    const auto malformedDamageResult =
        service.openDamage("data/damage.ini", malformedDamage);
    require(malformedDamageResult.status == AuthoringServiceStatus::invalid &&
                !malformedDamageResult.diagnostics.empty() &&
                malformedDamageResult.diagnostics.front().line != 0U &&
                service.exportDamageIni().text == damageExport.text,
            "truncated damage input keeps source context and cannot replace the baseline");
    const std::vector<std::uint8_t> malformedBottom{
        '[', 'C', 'O', 'L', 'L', 'I', 'D', 'E', 'R', '_', '2', ']', '\n',
        'C', 'E', 'N', 'T', 'R', 'E', '=', '0', ',', '\\'};
    const auto malformedBottomResult =
        service.openBottomColliders("data/colliders.ini", malformedBottom);
    require(malformedBottomResult.status == AuthoringServiceStatus::invalid &&
                !malformedBottomResult.diagnostics.empty() &&
                malformedBottomResult.diagnostics.front().message.find("at 0x") !=
                    std::string::npos &&
                malformedBottomResult.diagnostics.front().message.find("data/colliders.ini") !=
                    std::string::npos &&
                service.exportBottomCollidersIni().text == bottomExport.text,
            "truncated bottom-collider input keeps source context and cannot replace the baseline");

    AuthoringService missing;
    require(missing.openPrimary("car.kn5", primary).ok() &&
                missing.openCollider("collider.kn5", collider).ok(),
            "missing-identity service opens its assets");
    AuthoringTransaction missingIdentity;
    missingIdentity.label = "unbound collider edit";
    missingIdentity.operations = {SetColliderEdit{"0", colliderEdit}};
    require(missing.commit(missingIdentity).ok() &&
                missing.exportColliderKn5().status == AuthoringServiceStatus::unbound,
            "secondary edits without an identity fail closed");

    AuthoringService stale;
    require(stale.openPrimary("car.kn5", primary).ok() &&
                stale.openCollider("collider.kn5", collider).ok(),
            "stale service opens its original collider");
    const auto originalIdentity = stale.openCollider("collider.kn5", collider).identity;
    require(originalIdentity.has_value(), "original collider identity is retained");
    AuthoringTransaction staleBind;
    staleBind.label = "bind stale collider";
    staleBind.operations = {SetColliderAsset{*originalIdentity}, SetColliderEdit{"0", colliderEdit}};
    require(stale.commit(staleBind).ok() &&
                stale.openCollider("collider.kn5", changedCollider).ok() &&
                stale.exportColliderKn5().status == AuthoringServiceStatus::stale,
            "changed secondary bytes fail closed as stale");
}

}  // namespace

int main() {
    try {
        opensPrimaryAndKeepsStateAtomic();
        projectRoundTripUndoRedoAndExports();
        secondaryAssetsBindAndFailClosed();
        std::cout << "authoring_service_tests: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "authoring_service_tests: " << error.what() << '\n';
        return 1;
    }
}
