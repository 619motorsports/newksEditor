#include "apex/authoring/project_io.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using apex::authoring::AuthoringTransaction;
using apex::authoring::MaterialResource;
using apex::authoring::MaterialVector;
using apex::authoring::ProjectIoLimits;
using apex::authoring::ProjectSession;
using apex::authoring::SetDamageEdit;
using apex::authoring::SetMaterialResourceEdit;
using apex::authoring::SetMaterialScalarEdit;
using apex::authoring::SetMaterialVectorEdit;
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
    edits.operations = {
        SetNodeEdit{"0", node},
        SetWorkspaceFileEdit{2, [] { apex::authoring::WorkspaceFileEdit value; value.position = apex::authoring::Vector3{1.0F, 2.0F, 3.0F}; value.lodIn = 15.0F; return value; }()},
        SetMaterialScalarEdit{"Body", "shader", std::string("ksPerPixel")},
        SetMaterialVectorEdit{"Body", "ksDiffuse", MaterialVector{{1.0F, 0.5F, 0.0F, 1.0F}, 4}},
        SetMaterialResourceEdit{"Body", "txDiffuse", MaterialResource{false, std::string("textures/body.dds"), {}, {}}},
        SetSurfaceEdit{0, [] { apex::authoring::SurfaceEdit value; value.key = "TARMAC"; value.friction = 1.05F; return value; }()},
        SetDamageEdit{"VISUAL_OBJECT_0", [] { apex::authoring::DamageEdit value; value.minSpeed = 25.0F; value.damageZone = "FRONT"; return value; }()}
    };
    session.commit(edits);
    const auto json = apex::authoring::serializeProject(session.state());
    require(json.find("\"format\": \"apex-editor-project\"") != std::string::npos, "project format output");
    const auto loaded = apex::authoring::parseProject(json, identity());
    require(loaded.project.has_value() && loaded.diagnostics.empty(), "project JSON round trip");
    require(loaded.project->nodes.at("0").active == false && loaded.project->workspaceFiles.at(2).lodIn == 15.0F,
            "node and workspace reload");
    require(loaded.project->materials.at("Body").resources.at("txDiffuse").texture == "textures/body.dds",
            "material resource reload");
    require(loaded.project->damage.at("VISUAL_OBJECT_0").minSpeed == 25.0F, "damage reload");
}

void exportsDeterministicCspAndReportsUnsupported() {
    ProjectSession session(identity());
    session.commit({"materials", {
        SetMaterialScalarEdit{"Zed", "shader", std::string("ksPerPixel")},
        SetMaterialScalarEdit{"Zed", "blendMode", std::string("ALPHA_BLEND")},
        SetMaterialScalarEdit{"Zed", "zeta", 2.0F},
        SetMaterialVectorEdit{"Zed", "ksDiffuse", MaterialVector{{1.0F, 0.5F, 0.0F, 1.0F}, 4}},
        SetMaterialResourceEdit{"Zed", "txDiffuse", MaterialResource{false, std::string("body.dds"), {}, {}}},
        SetMaterialScalarEdit{"Alpha", "shader", std::string("ksPerPixel")}
    }});
    std::vector<apex::authoring::ProjectIoDiagnostic> diagnostics;
    const auto csp = apex::authoring::serializeEditorCsp(session.state(), {}, &diagnostics);
    require(csp.find("SHADER_REPLACEMENT_APEX_EDITOR_000") != std::string::npos &&
                csp.find("SHADER_REPLACEMENT_APEX_EDITOR_001") != std::string::npos,
            "CSP section ordering");
    require(csp.find("MATERIALS = Zed") != std::string::npos && csp.find("PROP_0 = ksDiffuse, 1, 0.5, 0, 1") != std::string::npos,
            "CSP material fields");
    require(csp.find("PROP_0 = ksDiffuse") < csp.find("PROP_1 = zeta, 2"), "CSP scalar/vector properties share sorted order");
    require(diagnostics.empty(), "modeled CSP fields have no diagnostics");
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
}

void reportsUnsupportedFieldsAndStaleSource() {
    const std::string json = "{\"format\":\"apex-editor-project\",\"version\":1,\"asset\":{\"name\":\"car.kn5\",\"size\":42,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\"meshEdits\":{\"BODY\":{\"layer\":2}},\"skinEdits\":{\"red\":{\"skinname\":\"Rosso\"}}}";
    const auto result = apex::authoring::parseProject(json, identity("other.kn5"));
    require(result.project.has_value(), "unsupported fields still preserve modeled project");
    require(result.diagnostics.size() >= 3, "unsupported and stale diagnostics");
}

void rejectsDirectInvalidStateAndPreservesJsonUnicodeRules() {
    apex::authoring::ProjectState direct;
    direct.source = identity();
    direct.materials["Body"].scalars["shader"] = 1.0F;
    bool rejected = false;
    try { (void)apex::authoring::serializeProject(direct); }
    catch (const apex::core::ParseError& error) { rejected = error.code() == "FIELD_TYPE"; }
    require(rejected, "direct material state fields must remain strings");

    direct.materials["Body"].scalars["shader"] = std::string("ksPerPixel");
    direct.workspaceFiles[0].name = "../escape.kn5";
    rejected = false;
    try { (void)apex::authoring::serializeProject(direct); }
    catch (const apex::core::ParseError& error) { rejected = error.code() == "RECOVERY_INVALID" || error.code() == "EDIT_INVALID"; }
    require(rejected, "direct workspace paths must be confined");

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

} // namespace

int main() {
    try {
        roundTripsModeledFields();
        exportsDeterministicCspAndReportsUnsupported();
        rejectsUntrustedJson();
        reportsUnsupportedFieldsAndStaleSource();
        rejectsDirectInvalidStateAndPreservesJsonUnicodeRules();
        enforcesFieldSchemasNullsAndSafeIntegers();
        std::cout << "project IO tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "project IO tests failed: " << error.what() << '\n';
        return 1;
    }
}
