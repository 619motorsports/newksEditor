#include "apex/authoring/project_io.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using apex::authoring::AuthoringTransaction;
using apex::authoring::GeometryEdit;
using apex::authoring::MaterialResource;
using apex::authoring::MaterialVector;
using apex::authoring::MeshEdit;
using apex::authoring::ProjectIoLimits;
using apex::authoring::ProjectSession;
using apex::authoring::SetDamageEdit;
using apex::authoring::SetDamageAsset;
using apex::authoring::SetColliderAsset;
using apex::authoring::SetColliderEdit;
using apex::authoring::SetBottomColliderAsset;
using apex::authoring::SetBottomColliderEdit;
using apex::authoring::SetGeometryEdit;
using apex::authoring::SetMaterialResourceEdit;
using apex::authoring::SetMaterialScalarEdit;
using apex::authoring::SetMaterialVectorEdit;
using apex::authoring::SetMeshEdit;
using apex::authoring::SetNodeEdit;
using apex::authoring::SetSurfaceEdit;
using apex::authoring::SetWorkspaceFileEdit;
using apex::authoring::SourceIdentity;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

SourceIdentity identity(std::string name = "car.kn5") { return {std::move(name), 42, std::string(64, 'a'), 6}; }

void roundTripsModeledFields() {
    ProjectSession session(identity());
    AuthoringTransaction edits;
    edits.label = "io fixture";
    apex::authoring::NodeEdit node;
    node.name = "BODY";
    node.active = false;
    MeshEdit mesh;
    mesh.transparent = true;
    mesh.castShadows = false;
    mesh.layer = 2;
    mesh.lodIn = 12.5F;
    GeometryEdit geometry;
    geometry.reverse_winding = true;
    geometry.transform =
        apex::authoring::Matrix4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1};
    SourceIdentity colliderAsset{"COLLIDER\\//collider.kn5", 128, std::string(64, 'B'), 7};
    SourceIdentity damageAsset{"data\\damage.ini", 32, std::string(64, 'C'), std::nullopt};
    SourceIdentity bottomColliderAsset{"data//colliders.ini", 24, std::string(64, 'D'), std::nullopt};
    apex::authoring::ColliderEdit collider;
    collider.reverseWinding = true;
    apex::authoring::BottomColliderEdit bottomCollider;
    bottomCollider.groundEnabled = false;
    edits.operations = {
        SetNodeEdit{"0", node},
        SetMeshEdit{"BODY", mesh},
        SetGeometryEdit{"0", geometry},
        SetWorkspaceFileEdit{2, [] { apex::authoring::WorkspaceFileEdit value; value.position = apex::authoring::Vector3{1.0F, 2.0F, 3.0F}; value.lodIn = 15.0F; return value; }()},
        SetMaterialScalarEdit{"Body", "shader", std::string("ksPerPixel")},
        SetMaterialScalarEdit{"Body", "emptyNumeric", std::string("")},
        SetMaterialVectorEdit{"Body", "ksDiffuse", MaterialVector{{1.0F, 0.5F, 0.0F, 1.0F}, 4}},
        SetMaterialResourceEdit{"Body", "txDiffuse", MaterialResource{false, std::string("textures/body.dds"), {}, {}}},
        SetSurfaceEdit{0, [] { apex::authoring::SurfaceEdit value; value.key = "TARMAC"; value.friction = 1.05F; return value; }()},
        SetColliderAsset{colliderAsset},
        SetColliderEdit{"0", collider},
        SetDamageAsset{damageAsset},
        SetBottomColliderAsset{bottomColliderAsset},
        SetBottomColliderEdit{0, bottomCollider},
        SetDamageEdit{"VISUAL_OBJECT_0", [] { apex::authoring::DamageEdit value; value.minSpeed = 25.0F; value.damageZone = "FRONT"; return value; }()}
    };
    session.commit(edits);
    const auto json = apex::authoring::serializeProject(session.state());
    require(json.find("\"format\": \"apex-editor-project\"") != std::string::npos, "project format output");
    require(json.find("\"colliderAsset\": {") != std::string::npos &&
                json.find("\"damageAsset\": {") != std::string::npos &&
                json.find("\"bottomColliderAsset\": {") != std::string::npos,
            "secondary identity output");
    const auto loaded = apex::authoring::parseProject(json, identity());
    require(loaded.project.has_value() && loaded.diagnostics.empty(), "project JSON round trip");
    require(loaded.project->nodes.at("0").active == false && loaded.project->workspaceFiles.at(2).lodIn == 15.0F,
            "node and workspace reload");
    require(loaded.project->meshes.at("BODY").transparent == true &&
                loaded.project->meshes.at("BODY").castShadows == false &&
                loaded.project->meshes.at("BODY").layer == 2 &&
                loaded.project->meshes.at("BODY").lodIn == 12.5F,
            "mesh reload");
    require(loaded.project->geometry.at("0").reverse_winding &&
                loaded.project->geometry.at("0").transform->at(12) == 2.0F,
            "geometry reload");
    require(loaded.project->materials.at("Body").resources.at("txDiffuse").texture == "textures/body.dds",
            "material resource reload");
    require(std::get<std::string>(loaded.project->materials.at("Body").scalars.at("emptyNumeric")).empty(),
            "empty numeric property string round trip");
    require(loaded.project->damage.at("VISUAL_OBJECT_0").minSpeed == 25.0F, "damage reload");
    require(loaded.project->colliderAsset->name == "COLLIDER/collider.kn5" &&
                loaded.project->colliderAsset->sha256 == std::string(64, 'b') &&
                loaded.project->colliderAsset->kn5Version == 7 &&
                loaded.project->damageAsset->name == "data/damage.ini" &&
                loaded.project->bottomColliderAsset->name == "data/colliders.ini",
            "secondary identities reload canonically");
}

void exportsDeterministicCspAndReportsUnsupported() {
    ProjectSession session(identity());
    session.commit({"materials", {
        SetMaterialScalarEdit{"Zed", "shader", std::string("ksPerPixel")},
        SetMaterialScalarEdit{"Zed", "blendMode", std::string("ALPHA_BLEND")},
        SetMaterialScalarEdit{"Zed", "zeta", 2.0F},
        SetMaterialVectorEdit{"Zed", "ksDiffuse", MaterialVector{{1.0F, 0.5F, 0.0F, 1.0F}, 4}},
        SetMaterialResourceEdit{"Zed", "txDiffuse", MaterialResource{false, std::string("body.dds"), {}, {}}},
        SetMaterialScalarEdit{"Alpha", "shader", std::string("ksPerPixel")},
        SetMeshEdit{"BODY MAIN", [] {
            MeshEdit value;
            value.transparent = false;
            value.castShadows = true;
            value.layer = 3;
            value.lodIn = 10.5F;
            value.lodOut = 50.0F;
            return value;
        }()}
    }});
    std::vector<apex::authoring::ProjectIoDiagnostic> diagnostics;
    const auto csp = apex::authoring::serializeEditorCsp(session.state(), {}, &diagnostics);
    require(csp.find("SHADER_REPLACEMENT_APEX_EDITOR_000") != std::string::npos &&
                csp.find("SHADER_REPLACEMENT_APEX_EDITOR_001") != std::string::npos,
            "CSP section ordering");
    require(csp.find("MATERIALS = Zed") != std::string::npos && csp.find("PROP_0 = ksDiffuse, 1, 0.5, 0, 1") != std::string::npos,
            "CSP material fields");
    require(csp.find("PROP_0 = ksDiffuse") < csp.find("PROP_1 = zeta, 2"), "CSP scalar/vector properties share sorted order");
    require(csp.find("[MESH_ADJUSTMENT_APEX_EDITOR_000]") != std::string::npos &&
                csp.find("MESHES = \"BODY MAIN\"") != std::string::npos &&
                csp.find("IS_TRANSPARENT = 0") != std::string::npos &&
                csp.find("LAYER = 3") != std::string::npos &&
                csp.find("LOD_IN = 10.5") != std::string::npos &&
                csp.find("LOD_OUT = 50") != std::string::npos &&
                csp.find("CAST_SHADOWS = 1") != std::string::npos,
            "CSP mesh fields match the production project exporter");
    require(diagnostics.empty(), "modeled CSP fields have no diagnostics");

    ProjectSession meshOnly(identity());
    MeshEdit transparent;
    transparent.transparent = true;
    meshOnly.commit({"mesh only", {SetMeshEdit{"BODY", transparent}}});
    const auto meshOnlyCsp = apex::authoring::serializeEditorCsp(meshOnly.state());
    require(meshOnlyCsp.find("modified.\n\n[MESH_ADJUSTMENT_APEX_EDITOR_000]") != std::string::npos,
            "mesh-only CSP output keeps the production section separator");

    session.commit({"node", {SetNodeEdit{"0", [] { apex::authoring::NodeEdit value; value.active = false; return value; }()}}});
    diagnostics.clear();
    (void)apex::authoring::serializeEditorCsp(session.state(), {}, &diagnostics);
    require(!diagnostics.empty() && diagnostics[0].code == "UNSUPPORTED_FIELD", "unsupported CSP diagnostic");
}

template <typename Function>
void expectsIoError(Function&& function, std::string_view code) {
    try {
        function();
    } catch (const apex::core::ParseError& error) {
        require(error.code() == code, "unexpected project IO error code");
        return;
    }
    throw std::runtime_error("malformed project JSON was accepted");
}

void rejectsUntrustedJson() {
    expectsIoError([] { (void)apex::authoring::parseProject("{\"format\":\"apex-editor-project\",\"format\":\"apex-editor-project\"}"); }, "JSON_DUPLICATE_KEY");
    expectsIoError([] { (void)apex::authoring::parseProject("{\"format\":\"apex-editor-project\",\"version\":1"); }, "JSON_TRUNCATED");
    expectsIoError([] { (void)apex::authoring::parseProject(
            "{\"format\":\"apex-editor-project\",\"version\":1,"
            "\"geometryEdits\":{\"0\":{\"transform\":[1,0");
    }, "JSON_TRUNCATED");
    const auto unsafe = apex::authoring::parseProject("{\"format\":\"apex-editor-project\",\"version\":1,\"asset\":{\"name\":\"car.kn5\",\"size\":1,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\"nodeEdits\":{\"../0\":{\"active\":true}}}");
    require(unsafe.project.has_value() && !unsafe.diagnostics.empty() && unsafe.diagnostics[0].code == "EDIT_INVALID", "unsafe path diagnostic");
    expectsIoError([] { (void)apex::authoring::parseProject("{\"format\":\"apex-editor-project\",\"version\":1,\"asset\":{\"name\":\"car.kn5\",\"size\":1,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\"nodeEdits\":{\"0\":{\"active\":NaN}}}"); }, "JSON_MALFORMED");
    ProjectIoLimits limits; limits.maxInputBytes = 10;
    expectsIoError([&] { (void)apex::authoring::parseProject("{\"format\":\"apex-editor-project\"}", limits); }, "INPUT_LIMIT");
    limits = {};
    limits.maxDepth = 0;
    expectsIoError([&] { (void)apex::authoring::parseProject("{\"format\":\"apex-editor-project\",\"version\":1}", limits); }, "JSON_DEPTH_LIMIT");
    limits = {};
    limits.maxMembers = 1;
    expectsIoError([&] { (void)apex::authoring::parseProject("{\"format\":\"apex-editor-project\",\"version\":1}", limits); }, "JSON_MEMBER_LIMIT");
    limits = {};
    limits.maxStringBytes = 4;
    expectsIoError([&] { (void)apex::authoring::parseProject("{\"format\":\"apex-editor-project\",\"version\":1}", limits); }, "JSON_STRING_LIMIT");
    limits = {};
    limits.maxEdits = 1;
    const auto limited = apex::authoring::parseProject("{\"format\":\"apex-editor-project\",\"version\":1,\"asset\":{\"name\":\"car.kn5\",\"size\":1,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\"nodeEdits\":{\"0\":{\"active\":true},\"1\":{\"active\":false}}}", limits);
    require(limited.project.has_value() && !limited.diagnostics.empty(), "edit count limit diagnostic");

    const std::string base = "{\"format\":\"apex-editor-project\",\"version\":1,\"asset\":{\"name\":\"car.kn5\",\"size\":1,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},";
    const auto malformedMesh = apex::authoring::parseProject(base + "\"meshEdits\":{\"BODY\":{\"layer\":-1,\"lodIn\":1e100,\"castShadows\":1}}}");
    require(malformedMesh.project.has_value() && malformedMesh.project->meshes.empty() && malformedMesh.diagnostics.size() >= 4,
            "malformed mesh edit is bounded and diagnosed");
    const auto malformedGeometry = apex::authoring::parseProject(base + "\"geometryEdits\":{\"01\":{\"reverseWinding\":true},\"0\":{\"transform\":[1,0,0]}}}");
    require(malformedGeometry.project.has_value() && malformedGeometry.project->geometry.empty() && malformedGeometry.diagnostics.size() >= 2,
            "malformed geometry edits are bounded and diagnosed");
}

void reportsUnsupportedFieldsAndStaleSource() {
    const std::string json = "{\"format\":\"apex-editor-project\",\"version\":1,\"asset\":{\"name\":\"car.kn5\",\"size\":42,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\"meshEdits\":{\"BODY\":{\"layer\":2}},\"skinEdits\":{\"red\":{\"skinname\":\"Rosso\"}}}";
    const auto result = apex::authoring::parseProject(json, identity("other.kn5"));
    require(result.project.has_value(), "unsupported fields still preserve modeled project");
    require(result.project->meshes.at("BODY").layer == 2,
            "mesh field is modeled");
    require(result.diagnostics.size() >= 2, "unsupported and stale diagnostics");
}

void rejectsDirectInvalidStateAndPreservesJsonUnicodeRules() {
    apex::authoring::ProjectState direct;
    direct.source = identity();
    direct.materials["Body"].scalars["shader"] = 1.0F;
    bool rejected = false;
    try { (void)apex::authoring::serializeProject(direct); }
    catch (const apex::core::ParseError& error) {
        rejected = error.code() == "FIELD_TYPE" || error.code() == "EDIT_INVALID";
    }
    require(rejected, "direct material state fields must remain strings");

    direct.materials["Body"].scalars["shader"] = std::string("ksPerPixel");
    direct.materials["Body"].scalars["useDetail"] = true;
    rejected = false;
    try { (void)apex::authoring::serializeProject(direct); }
    catch (const apex::core::ParseError& error) { rejected = error.code() == "EDIT_INVALID"; }
    require(rejected, "boolean material properties cannot create non-round-trippable state");

    direct.materials["Body"].scalars.erase("useDetail");
    direct.workspaceFiles[0].name = "../escape.kn5";
    rejected = false;
    try { (void)apex::authoring::serializeProject(direct); }
    catch (const apex::core::ParseError& error) { rejected = error.code() == "RECOVERY_INVALID" || error.code() == "EDIT_INVALID"; }
    require(rejected, "direct workspace paths must be confined");

    direct.workspaceFiles.clear();
    direct.geometry["01"].reverse_winding = true;
    rejected = false;
    try { (void)apex::authoring::serializeProject(direct); }
    catch (const apex::core::ParseError& error) { rejected = error.code() == "RECOVERY_INVALID" || error.code() == "EDIT_INVALID"; }
    require(rejected, "direct geometry paths must be canonical");

    const std::string prefix = "{\"format\":\"apex-editor-project\",\"version\":1,\"asset\":{\"name\":\"car.kn5\",\"size\":42,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\"nodeEdits\":{\"0\":{\"name\":\"";
    const auto unicode = apex::authoring::parseProject(prefix + "\\uD83D\\uDE80\"}}}", identity());
    require(unicode.project.has_value() && unicode.diagnostics.empty(), "surrogate pair is decoded");
    expectsIoError([&] { (void)apex::authoring::parseProject(prefix + "\\uD83D\"}}}", identity()); }, "JSON_MALFORMED");
}

void enforcesFieldSchemasNullsAndSafeIntegers() {
    const std::string base = "{\"format\":\"apex-editor-project\",\"version\":1,\"asset\":{\"name\":\"car.kn5\",\"size\":42,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},";
    const auto damage = apex::authoring::parseProject(base + "\"damageEdits\":{\"VISUAL_OBJECT_0\":{\"initialLevel\":25}}}", identity());
    require(!damage.project->damage.contains("VISUAL_OBJECT_0"), "section-invalid damage field is dropped");
    require(!damage.diagnostics.empty() && damage.diagnostics.front().code == "UNSUPPORTED_FIELD", "section-invalid damage field is diagnosed");
    const auto nullWav = apex::authoring::parseProject(base + "\"workspaceEdits\":{\"files\":{\"0\":{\"playWav\":null}}}}", identity());
    require(!nullWav.diagnostics.empty() && nullWav.diagnostics.front().code == "UNSUPPORTED_NULL", "null playWav is explicitly staged");
    const std::string unsafeSize = "{\"format\":\"apex-editor-project\",\"version\":1,\"asset\":{\"name\":\"car.kn5\",\"size\":9007199254740992,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}";
    const auto sizeResult = apex::authoring::parseProject(unsafeSize, identity());
    require(sizeResult.project.has_value() && !sizeResult.diagnostics.empty() && sizeResult.diagnostics.front().code == "SOURCE_SIZE", "unsafe source size is diagnosed");
}

void handlesUntrustedSecondaryIdentities() {
    const std::string prefix =
        "{\"format\":\"apex-editor-project\",\"version\":1,"
        "\"asset\":{\"name\":\"car.kn5\",\"size\":42,"
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},";
    const std::string colliderEdits = "\"colliderEdits\":{\"0\":{\"reverseWinding\":true}}";

    const auto malformed = apex::authoring::parseProject(
        prefix + "\"colliderAsset\":{\"name\":\"collider.kn5\",\"size\":128,\"sha256\":\"short\"}," +
        colliderEdits + "}", identity());
    require(malformed.project.has_value() && malformed.project->colliders.contains("0") &&
                !malformed.project->colliderAsset && !malformed.diagnostics.empty(),
            "malformed secondary identity is diagnosed while edits remain stale");

    const auto wrongType = apex::authoring::parseProject(
        prefix + "\"colliderAsset\":7," + colliderEdits + "}", identity());
    require(wrongType.project.has_value() && !wrongType.project->colliderAsset &&
                !wrongType.diagnostics.empty() && wrongType.diagnostics.front().code == "FIELD_TYPE",
            "wrong-type secondary identity is diagnosed");

    const auto unsafeSize = apex::authoring::parseProject(
        prefix + "\"colliderAsset\":{\"name\":\"collider.kn5\",\"size\":9007199254740992,"
                 "\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}," +
        colliderEdits + "}", identity());
    require(unsafeSize.project.has_value() && !unsafeSize.project->colliderAsset &&
                !unsafeSize.diagnostics.empty(),
            "unsafe secondary identity size is diagnosed");

    const auto missing = apex::authoring::parseProject(prefix + colliderEdits + "}", identity());
    require(missing.project.has_value() && missing.project->colliders.contains("0") &&
                !missing.project->colliderAsset && missing.diagnostics.empty(),
            "legacy edits without identity remain loaded but unbound");

    const auto orphan = apex::authoring::parseProject(
        prefix + "\"colliderAsset\":{\"name\":\"collider.kn5\",\"size\":128,"
                 "\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}}",
        identity());
    require(orphan.project.has_value() && !orphan.project->colliderAsset && orphan.diagnostics.empty(),
            "orphan secondary identity normalizes to null");
    const auto orphanJson = apex::authoring::serializeProject(*orphan.project);
    require(orphanJson.find("\"colliderAsset\": null") != std::string::npos,
            "orphan secondary identity serializes as null");

    expectsIoError([&] {
        (void)apex::authoring::parseProject(
            prefix + "\"colliderAsset\":{\"name\":\"collider.kn5\",\"size\":128", identity());
    }, "JSON_TRUNCATED");
}

void preservesStableColliderPathsAndRejectsMalformedEdits() {
    const std::string prefix =
        "{\"format\":\"apex-editor-project\",\"version\":1,"
        "\"asset\":{\"name\":\"car.kn5\",\"size\":42,"
        "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},";
    const auto nested = apex::authoring::parseProject(
        prefix + "\"colliderEdits\":{\"0/1\":{\"reverseWinding\":true}}}", identity());
    require(nested.project.has_value() && nested.project->colliders.contains("0/1") &&
                nested.diagnostics.empty(),
            "collider edit retains its stable nested hierarchy path");
    const auto nestedJson = apex::authoring::serializeProject(*nested.project);
    require(nestedJson.find("\"0/1\": {") != std::string::npos,
            "nested collider path survives project serialization");

    const auto malformedPath = apex::authoring::parseProject(
        prefix + "\"colliderEdits\":{\"0//1\":{\"reverseWinding\":true}}}", identity());
    require(malformedPath.project.has_value() && malformedPath.project->colliders.empty() &&
                !malformedPath.diagnostics.empty() && malformedPath.diagnostics.front().code == "EDIT_INVALID",
            "non-canonical collider path is rejected without a partial edit");
    const auto whitespacePath = apex::authoring::parseProject(
        prefix + "\"colliderEdits\":{\" 0\":{\"reverseWinding\":true}}}", identity());
    require(whitespacePath.project.has_value() && whitespacePath.project->colliders.empty() &&
                !whitespacePath.diagnostics.empty() && whitespacePath.diagnostics.front().code == "EDIT_INVALID",
            "collider path whitespace is rejected instead of retargeting an edit");
    const auto falseOnly = apex::authoring::parseProject(
        prefix + "\"colliderEdits\":{\"0\":{\"reverseWinding\":false}}}", identity());
    require(falseOnly.project.has_value() && falseOnly.project->colliders.empty() &&
                !falseOnly.diagnostics.empty() && falseOnly.diagnostics.front().code == "EDIT_INVALID",
            "false-only collider topology edit normalizes to an empty rejected edit");
    expectsIoError([&] {
        (void)apex::authoring::parseProject(
            prefix + "\"colliderEdits\":{\"0/1\":{\"transform\":[1,0", identity());
    }, "JSON_TRUNCATED");
}

} // namespace

int main() {
    try {
        roundTripsModeledFields();
        exportsDeterministicCspAndReportsUnsupported();
        rejectsUntrustedJson();
        reportsUnsupportedFieldsAndStaleSource();
        rejectsDirectInvalidStateAndPreservesJsonUnicodeRules();
        enforcesFieldSchemasNullsAndSafeIntegers();
        handlesUntrustedSecondaryIdentities();
        preservesStableColliderPathsAndRejectsMalformedEdits();
        std::cout << "project IO tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "project IO tests failed: " << error.what() << '\n';
        return 1;
    }
}
