#include "apex/authoring/project.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

using apex::authoring::AuthoringError;
using apex::authoring::AuthoringLimits;
using apex::authoring::AuthoringTransaction;
using apex::authoring::BottomColliderEdit;
using apex::authoring::ClearNodeEdit;
using apex::authoring::EditOperation;
using apex::authoring::MaterialResource;
using apex::authoring::MaterialVector;
using apex::authoring::ProjectSession;
using apex::authoring::RecoverySnapshot;
using apex::authoring::SetBottomColliderEdit;
using apex::authoring::SetDamageEdit;
using apex::authoring::SetMaterialResourceEdit;
using apex::authoring::SetMaterialScalarEdit;
using apex::authoring::SetMaterialVectorEdit;
using apex::authoring::SetNodeEdit;
using apex::authoring::SetSurfaceEdit;
using apex::authoring::SetWorkspaceFileEdit;
using apex::authoring::SourceIdentity;
using apex::authoring::sourceIdentityMatches;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

SourceIdentity identity(std::string name = "car.kn5") {
    return {std::move(name), 42, std::string(64, 'a'), 6};
}

apex::authoring::NodeEdit nodeEdit(std::optional<bool> active = std::nullopt,
                                   std::optional<std::string> name = std::nullopt) {
    apex::authoring::NodeEdit output;
    output.active = active;
    output.name = std::move(name);
    return output;
}

template <typename Function>
void expectsError(Function&& function, std::string_view code) {
    try {
        function();
    } catch (const AuthoringError& error) {
        require(error.code() == code, "unexpected authoring error code");
        return;
    }
    throw std::runtime_error("malformed authoring input was accepted");
}

void validatesIdentityAndStablePaths() {
    const auto normalized = apex::authoring::normalizeSourceIdentity({"CAR\\body.kn5", 42,
                                                                        std::string(64, 'A'), 6});
    require(normalized.name == "CAR/body.kn5" && normalized.sha256 == std::string(64, 'a'),
            "source identity normalization");
    require(sourceIdentityMatches(normalized, identity("car/body.kn5")), "case-insensitive identity match");
    require(!sourceIdentityMatches(normalized, identity("other.kn5")), "stale identity mismatch");
    expectsError([] { (void)apex::authoring::normalizeSourceIdentity({"car.kn5", 1, "truncated", 6}); },
                 "SOURCE_IDENTITY_INVALID");
    ProjectSession session(identity());
    expectsError([&] {
        session.commit({"unsafe", {SetNodeEdit{"../0", nodeEdit(true)}}});
    }, "EDIT_INVALID");
    expectsError([&] {
        session.commit({"noncanonical", {SetNodeEdit{"01", nodeEdit(true)}}});
    }, "EDIT_INVALID");
}

void appliesTypedTransactionWithoutMutatingSource() {
    ProjectSession session(identity());
    AuthoringTransaction transaction;
    transaction.label = "authoring batch";
    transaction.operations = {
        SetNodeEdit{"0", {std::string("BODY"), false,
                                apex::authoring::Matrix4{1, 0, 0, 0, 0, 1, 0, 0,
                                                         0, 0, 1, 0, 1, 2, 3, 1}}},
        SetWorkspaceFileEdit{2, [] {
            apex::authoring::WorkspaceFileEdit output;
            output.position = apex::authoring::Vector3{1, 2, 3};
            output.lodIn = 15.0F;
            return output;
        }()},
        SetMaterialScalarEdit{"Body", "shader", std::string("ksPerPixel")},
        SetMaterialVectorEdit{"Body", "ksDiffuse", MaterialVector{{1, 0.5F, 0, 1}, 4}},
        SetMaterialResourceEdit{"Body", "txDiffuse", MaterialResource{false, std::string("textures/body.dds"), std::nullopt, std::nullopt}},
        SetSurfaceEdit{0, [] {
            apex::authoring::SurfaceEdit output;
            output.key = "TARMAC";
            output.friction = 1.05F;
            output.isPitlane = false;
            return output;
        }()},
        SetBottomColliderEdit{0, BottomColliderEdit{.centre = apex::authoring::Vector3{0, 1, 0},
                                                       .size = apex::authoring::Vector3{1, 2, 3},
                                                       .groundEnabled = true}},
        SetDamageEdit{"visual_object_0", [] {
            apex::authoring::DamageEdit output;
            output.initialLevel = 20.0F;
            output.name = "HOOD";
            output.damageZone = "front";
            return output;
        }()}
    };
    session.commit(transaction);
    const auto& state = session.state();
    require(state.nodes.at("0").active == false, "node active edit");
    require(state.workspaceFiles.at(2).lodIn == 15.0F, "workspace LOD edit");
    require(std::get<std::string>(state.materials.at("Body").scalars.at("shader")) == "ksPerPixel",
            "material scalar edit");
    require(state.materials.at("Body").vectors.at("ksDiffuse").components == 4,
            "material vector edit");
    require(state.materials.at("Body").resources.at("txDiffuse").texture == "textures/body.dds",
            "material resource edit");
    require(state.surfaces.at(0).key == "TARMAC" && state.bottomColliders.at(0).size->at(1) == 2.0F,
            "surface and collider edits");
    require(state.damage.at("VISUAL_OBJECT_0").damageZone == "FRONT", "damage edit");
    require(session.baselineState().nodes.empty() && session.baselineState().source.name == "car.kn5",
            "source baseline remains immutable");
}

void transactionsAreAtomicAndUndoable() {
    ProjectSession session(identity());
    const auto initialRevision = session.state().revision;
    expectsError([&] {
        session.commit({"atomic", {SetNodeEdit{"root/0", nodeEdit(false)},
                                     SetNodeEdit{"bad path", nodeEdit(true)}}});
    }, "EDIT_INVALID");
    require(session.state().revision == initialRevision && session.state().nodes.empty() && !session.canUndo(),
            "invalid transaction is atomic");

    session.commit({"one", {SetNodeEdit{"0", nodeEdit(false)}}});
    const auto firstRevision = session.state().revision;
    session.commit({"two", {SetNodeEdit{"0", nodeEdit(std::nullopt, std::string("BODY"))}}});
    const auto secondRevision = session.state().revision;
    require(secondRevision > firstRevision, "commit revisions are monotonic");
    require(session.undoCount() == 2 && session.state().nodes.at("0").name == "BODY", "transaction history");
    session.undo();
    const auto undoRevision = session.state().revision;
    require(undoRevision > secondRevision, "undo revision is monotonic");
    require(!session.state().nodes.at("0").name.has_value() && session.state().nodes.at("0").active == false,
            "undo restores prior state");
    session.redo();
    const auto redoRevision = session.state().revision;
    require(redoRevision > undoRevision, "redo revision is monotonic");
    require(session.state().nodes.at("0").name == "BODY", "redo restores edit");

    session.commit({"clear", {ClearNodeEdit{"0"}}});
    require(session.state().nodes.empty(), "clear operation");
    session.restoreBaseline();
    require(session.state().revision > redoRevision, "baseline revision is monotonic");
    require(!session.canRedo(), "baseline restore clears redo history");
}

void recoveryValidatesSourceAndLimits() {
    AuthoringLimits limits;
    limits.maxHistory = 1;
    limits.maxRecoveryEdits = 3;
    ProjectSession session(identity(), limits);
    session.commit({"one", {SetNodeEdit{"0", nodeEdit(false)}}});
    const RecoverySnapshot snapshot = session.recoverySnapshot();
    const auto stale = session.validateRecovery(snapshot, identity("other.kn5"));
    require(!stale.restored && !stale.diagnostics.empty() && stale.diagnostics[0].code == "STALE_SOURCE",
            "stale recovery diagnostic");
    const auto restored = session.recover(snapshot, identity());
    require(restored.restored && session.state().nodes.at("0").active == false, "valid recovery");

    session.commit({"second", {SetNodeEdit{"1", nodeEdit(true)}}});
    const auto tooLarge = session.recoverySnapshot();
    const auto limited = session.validateRecovery(tooLarge, identity());
    require(!limited.restored, "recovery edit limit");

    AuthoringLimits totalLimits;
    totalLimits.maxTotalEdits = 1;
    totalLimits.maxRecoveryEdits = 20;
    ProjectSession totalLimited(identity(), totalLimits);
    RecoverySnapshot oversized = totalLimited.recoverySnapshot();
    oversized.state.nodes.emplace("0", nodeEdit(false));
    oversized.state.nodes.emplace("1", nodeEdit(true));
    const auto totalResult = totalLimited.validateRecovery(oversized, identity());
    require(!totalResult.restored &&
                std::any_of(totalResult.diagnostics.begin(), totalResult.diagnostics.end(),
                            [](const auto& diagnostic) { return diagnostic.code == "EDIT_LIMIT"; }),
            "recovery honors total edit limit");

    ProjectSession recoverySession(identity("car/body.kn5"));
    RecoverySnapshot nonCanonical = recoverySession.recoverySnapshot();
    nonCanonical.state.source.name = "CAR\\BODY.KN5";
    nonCanonical.state.source.sha256 = std::string(64, 'A');
    const auto beforeRecovery = recoverySession.state().revision;
    const auto recovery = recoverySession.recover(nonCanonical, identity("car/body.kn5"));
    require(recovery.restored && recoverySession.state().source.name == "CAR/BODY.KN5" &&
                recoverySession.state().revision > beforeRecovery,
            "recovery normalizes and advances revision");
}

} // namespace

int main() {
    try {
        validatesIdentityAndStablePaths();
        appliesTypedTransactionWithoutMutatingSource();
        transactionsAreAtomicAndUndoable();
        recoveryValidatesSourceAndLimits();
        std::cout << "authoring project tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "authoring project tests failed: " << error.what() << '\n';
        return 1;
    }
}
