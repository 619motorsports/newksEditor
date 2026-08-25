#include "apex/app/authoring_service.hpp"

#include "apex/core/parse_error.hpp"
#include "apex/core/sha256.hpp"
#include "apex/formats/kn5_write.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace apex::app {
namespace {

using authoring::AuthoringTransaction;
using authoring::ProjectState;
using authoring::SourceIdentity;

[[nodiscard]] AuthoringServiceDiagnostic diagnostic(std::string_view code,
                                                    std::string_view path,
                                                    std::string_view message,
                                                    std::size_t line = 0) {
    return {std::string(code), std::string(path), std::string(message), line};
}

[[nodiscard]] AuthoringServiceResult failure(AuthoringServiceStatus status,
                                             std::string_view code,
                                             std::string_view path,
                                             std::string_view message) {
    AuthoringServiceResult result;
    result.status = status;
    result.diagnostics.push_back(diagnostic(code, path, message));
    return result;
}

template <typename Result>
void addException(Result& result, std::string_view code, std::string_view path,
                  const std::exception& error) {
    result.diagnostics.push_back(diagnostic(code, path, error.what()));
}

template <typename Result>
void addProjectDiagnostics(Result& result,
                           const std::vector<authoring::ProjectIoDiagnostic>& diagnostics) {
    for (const auto& item : diagnostics)
        result.diagnostics.push_back(diagnostic(item.code, item.path, item.message));
}

template <typename Result>
void addSecondaryDiagnostics(Result& result,
                             const std::vector<authoring::SecondaryAssetDiagnostic>& diagnostics) {
    for (const auto& item : diagnostics)
        result.diagnostics.push_back(diagnostic(item.code, item.path, item.message));
}

template <typename Result>
void addDamageDiagnostics(Result& result,
                          const std::vector<domain::CarDamageDiagnostic>& diagnostics,
                          std::string_view source) {
    for (const auto& item : diagnostics)
        result.diagnostics.push_back(
            diagnostic(item.code, source, item.message, item.line));
}

template <typename Result>
void addBottomDiagnostics(Result& result,
                          const std::vector<domain::BottomColliderParseDiagnostic>& diagnostics,
                          std::string_view source) {
    for (const auto& item : diagnostics)
        result.diagnostics.push_back(
            diagnostic(item.code, source, item.message, item.line));
}

template <typename Result>
void addSurfaceDiagnostics(
    Result& result,
    const std::vector<authoring::ProjectSurfacesDiagnostic>& diagnostics) {
    for (const auto& item : diagnostics)
        result.diagnostics.push_back(
            diagnostic(item.code, item.path, item.message, item.line));
}

template <typename Result>
void addWorkspaceDiagnostics(
    Result& result,
    const std::vector<authoring::ProjectWorkspaceDiagnostic>& diagnostics) {
    for (const auto& item : diagnostics)
        result.diagnostics.push_back(
            diagnostic(item.code, item.path, item.message, item.line));
}

[[nodiscard]] SourceIdentity identityFor(std::string name,
                                         std::span<const std::uint8_t> bytes,
                                         std::optional<std::uint32_t> kn5Version = std::nullopt) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()))
        throw core::ParseError("AUTHORING", name, 0, "SOURCE_SIZE",
                               "asset byte count exceeds the source identity range");
    SourceIdentity identity;
    identity.name = std::move(name);
    identity.size = static_cast<std::uint64_t>(bytes.size());
    identity.sha256 = core::sha256Hex(bytes);
    identity.kn5Version = kn5Version;
    return normalizeSourceIdentity(std::move(identity));
}

[[nodiscard]] SourceIdentity secondaryIdentityFor(
    std::string name, std::span<const std::uint8_t> bytes,
    std::optional<std::uint32_t> kn5Version = std::nullopt) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()))
        throw core::ParseError("AUTHORING", name, 0, "SOURCE_SIZE",
                               "asset byte count exceeds the source identity range");
    SourceIdentity identity;
    identity.name = std::move(name);
    identity.size = static_cast<std::uint64_t>(bytes.size());
    identity.sha256 = core::sha256Hex(bytes);
    identity.kn5Version = kn5Version;
    return normalizeSecondaryAssetIdentity(std::move(identity));
}

[[nodiscard]] std::string_view textBytes(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.empty()) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::string baseName(std::string_view source, std::string_view suffix) {
    const auto slash = source.find_last_of("/\\");
    std::string name(source.substr(slash == std::string_view::npos ? 0U : slash + 1U));
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    if (name.empty()) name = "asset";
    name += suffix;
    return name;
}

[[nodiscard]] std::string fileName(std::string_view source,
                                   std::string_view fallback) {
    const auto slash = source.find_last_of("/\\");
    std::string name(source.substr(slash == std::string_view::npos ? 0U : slash + 1U));
    return name.empty() ? std::string(fallback) : name;
}

[[nodiscard]] AuthoringServiceStatus secondaryStatus(
    authoring::SecondaryAssetStatus status) noexcept {
    switch (status) {
    case authoring::SecondaryAssetStatus::unchanged:
    case authoring::SecondaryAssetStatus::applied:
        return AuthoringServiceStatus::ok;
    case authoring::SecondaryAssetStatus::unbound:
        return AuthoringServiceStatus::unbound;
    case authoring::SecondaryAssetStatus::stale:
        return AuthoringServiceStatus::stale;
    case authoring::SecondaryAssetStatus::invalid:
        return AuthoringServiceStatus::invalid;
    }
    return AuthoringServiceStatus::failed;
}

[[nodiscard]] AuthoringServiceStatus surfacesStatus(
    authoring::ProjectSurfacesStatus status) noexcept {
    switch (status) {
    case authoring::ProjectSurfacesStatus::ok:
        return AuthoringServiceStatus::ok;
    case authoring::ProjectSurfacesStatus::invalid:
        return AuthoringServiceStatus::invalid;
    case authoring::ProjectSurfacesStatus::failed:
        return AuthoringServiceStatus::failed;
    }
    return AuthoringServiceStatus::failed;
}

[[nodiscard]] AuthoringServiceStatus workspaceStatus(
    authoring::ProjectWorkspaceStatus status) noexcept {
    switch (status) {
    case authoring::ProjectWorkspaceStatus::ok:
        return AuthoringServiceStatus::ok;
    case authoring::ProjectWorkspaceStatus::invalid:
        return AuthoringServiceStatus::invalid;
    case authoring::ProjectWorkspaceStatus::failed:
        return AuthoringServiceStatus::failed;
    }
    return AuthoringServiceStatus::failed;
}

template <typename Result>
void setRevision(Result& result, const std::unique_ptr<authoring::ProjectSession>& session) {
    result.revision = session ? session->state().revision : 0U;
}

}  // namespace

const char* authoring_service_status_name(AuthoringServiceStatus status) noexcept {
    switch (status) {
    case AuthoringServiceStatus::ok: return "ok";
    case AuthoringServiceStatus::not_open: return "not_open";
    case AuthoringServiceStatus::invalid: return "invalid";
    case AuthoringServiceStatus::stale: return "stale";
    case AuthoringServiceStatus::unbound: return "unbound";
    case AuthoringServiceStatus::unsupported: return "unsupported";
    case AuthoringServiceStatus::failed: return "failed";
    }
    return "failed";
}

AuthoringService::AuthoringService(AuthoringServiceLimits limits)
    : limits_(std::move(limits)) {}

AuthoringService::~AuthoringService() = default;
AuthoringService::AuthoringService(AuthoringService&&) noexcept = default;
AuthoringService& AuthoringService::operator=(AuthoringService&&) noexcept = default;

AuthoringServiceOpenResult AuthoringService::openPrimary(
    std::string name, std::span<const std::uint8_t> bytes) {
    AuthoringServiceOpenResult result;
    try {
        auto parseOptions = limits_.primary_parse;
        parseOptions.metadataOnly = false;
        auto model = formats::parseKn5(bytes, name, parseOptions);
        auto identity = identityFor(std::move(name), bytes, model.version);
        auto geometry = authoring::capture_static_geometry_baselines(
            model.root, limits_.geometry);
        auto session = std::make_unique<authoring::ProjectSession>(
            identity, limits_.project);

        primary_baseline_ = std::move(model);
        primary_geometry_baselines_ = std::move(geometry);
        session_ = std::move(session);
        collider_.reset();
        damage_.reset();
        bottom_colliders_.reset();
        surfaces_.reset();
        workspace_.reset();
        result.status = AuthoringServiceStatus::ok;
        result.identity = std::move(identity);
        setRevision(result, session_);
        return result;
    } catch (const core::ParseError& error) {
        result.status = AuthoringServiceStatus::invalid;
        result.diagnostics.push_back(
            diagnostic(error.code(), error.source(), error.what()));
    } catch (const std::exception& error) {
        result.status = AuthoringServiceStatus::invalid;
        addException(result, "PRIMARY_OPEN_FAILED", "primary", error);
    }
    return result;
}

AuthoringServiceResult AuthoringService::loadProject(std::string_view json) {
    if (!isOpen()) return failure(AuthoringServiceStatus::not_open, "NOT_OPEN", "project", "a primary KN5 is not open");

    AuthoringServiceResult result;
    try {
        const auto parsed = authoring::parseProject(
            json, session_->source(), limits_.project_io);
        if (!parsed.diagnostics.empty()) {
            result.status = AuthoringServiceStatus::invalid;
            addProjectDiagnostics(result, parsed.diagnostics);
            return result;
        }
        if (!parsed.project) {
            return failure(AuthoringServiceStatus::invalid, "PROJECT_INVALID",
                           "project", "project JSON did not produce a state");
        }

        // Recover into a completely separate session. The live session is
        // replaced only after every parse, identity, and state check passes.
        auto candidate = std::make_unique<authoring::ProjectSession>(
            session_->source(), limits_.project);
        const auto recovered = candidate->recover(
            authoring::RecoverySnapshot{*parsed.project}, session_->source());
        if (!recovered.restored) {
            result.status = AuthoringServiceStatus::invalid;
            for (const auto& item : recovered.diagnostics)
                result.diagnostics.push_back(
                    diagnostic(item.code, "project", item.message));
            return result;
        }
        session_ = std::move(candidate);
        result.status = AuthoringServiceStatus::ok;
        setRevision(result, session_);
    } catch (const core::ParseError& error) {
        result.status = AuthoringServiceStatus::invalid;
        result.diagnostics.push_back(
            diagnostic(error.code(), error.source(), error.what()));
    } catch (const std::exception& error) {
        result.status = AuthoringServiceStatus::invalid;
        addException(result, "PROJECT_LOAD_FAILED", "project", error);
    }
    return result;
}

AuthoringServiceResult AuthoringService::commit(
    const AuthoringTransaction& transaction) {
    if (!isOpen()) return failure(AuthoringServiceStatus::not_open, "NOT_OPEN", "project", "a primary KN5 is not open");
    AuthoringServiceResult result;
    try {
        (void)session_->commit(transaction);
        result.status = AuthoringServiceStatus::ok;
        setRevision(result, session_);
    } catch (const core::ParseError& error) {
        result.status = AuthoringServiceStatus::invalid;
        result.diagnostics.push_back(
            diagnostic(error.code(), error.source(), error.what()));
        setRevision(result, session_);
    } catch (const std::exception& error) {
        result.status = AuthoringServiceStatus::invalid;
        addException(result, "TRANSACTION_FAILED", "transaction", error);
        setRevision(result, session_);
    }
    return result;
}

AuthoringServiceResult AuthoringService::undo() {
    if (!isOpen()) return failure(AuthoringServiceStatus::not_open, "NOT_OPEN", "project", "a primary KN5 is not open");
    AuthoringServiceResult result;
    try {
        (void)session_->undo();
        result.status = AuthoringServiceStatus::ok;
    } catch (const std::exception& error) {
        result.status = AuthoringServiceStatus::failed;
        addException(result, "UNDO_FAILED", "project", error);
    }
    setRevision(result, session_);
    return result;
}

AuthoringServiceResult AuthoringService::redo() {
    if (!isOpen()) return failure(AuthoringServiceStatus::not_open, "NOT_OPEN", "project", "a primary KN5 is not open");
    AuthoringServiceResult result;
    try {
        (void)session_->redo();
        result.status = AuthoringServiceStatus::ok;
    } catch (const std::exception& error) {
        result.status = AuthoringServiceStatus::failed;
        addException(result, "REDO_FAILED", "project", error);
    }
    setRevision(result, session_);
    return result;
}

AuthoringServiceTextResult AuthoringService::saveProject() const {
    AuthoringServiceTextResult result;
    if (!isOpen()) {
        result.status = AuthoringServiceStatus::not_open;
        result.diagnostics.push_back(
            diagnostic("NOT_OPEN", "project", "a primary KN5 is not open"));
        return result;
    }
    result.suggested_name = baseName(session_->source().name, ".apex.json");
    result.revision = session_->state().revision;
    try {
        result.text = authoring::serializeProject(session_->state(), limits_.project_io);
        result.status = AuthoringServiceStatus::ok;
    } catch (const core::ParseError& error) {
        result.status = AuthoringServiceStatus::invalid;
        result.text.clear();
        result.diagnostics.push_back(
            diagnostic(error.code(), error.source(), error.what()));
    } catch (const std::exception& error) {
        result.status = AuthoringServiceStatus::failed;
        result.text.clear();
        addException(result, "PROJECT_SAVE_FAILED", "project", error);
    }
    return result;
}

AuthoringServiceTextResult AuthoringService::exportCsp() const {
    AuthoringServiceTextResult result;
    if (!isOpen()) {
        result.status = AuthoringServiceStatus::not_open;
        result.diagnostics.push_back(
            diagnostic("NOT_OPEN", "project", "a primary KN5 is not open"));
        return result;
    }
    result.suggested_name = baseName(session_->source().name, "_apex.ini");
    result.revision = session_->state().revision;
    std::vector<authoring::ProjectIoDiagnostic> diagnostics;
    try {
        auto text = authoring::serializeEditorCsp(
            session_->state(), limits_.project_io, &diagnostics);
        result.text = std::move(text);
        addProjectDiagnostics(result, diagnostics);
        result.status = AuthoringServiceStatus::ok;
    } catch (const core::ParseError& error) {
        result.status = AuthoringServiceStatus::invalid;
        result.text.clear();
        result.diagnostics.push_back(
            diagnostic(error.code(), error.source(), error.what()));
    } catch (const std::exception& error) {
        result.status = AuthoringServiceStatus::failed;
        result.text.clear();
        addException(result, "CSP_EXPORT_FAILED", "project", error);
    }
    return result;
}

AuthoringServiceBytesResult AuthoringService::exportPrimaryKn5() const {
    AuthoringServiceBytesResult result;
    if (!isOpen()) {
        result.status = AuthoringServiceStatus::not_open;
        result.diagnostics.push_back(
            diagnostic("NOT_OPEN", "project", "a primary KN5 is not open"));
        return result;
    }
    result.suggested_name = baseName(session_->source().name, "_apex.kn5");
    result.revision = session_->state().revision;
    if (primary_baseline_->encryption.has_value()) {
        result.status = AuthoringServiceStatus::unsupported;
        result.diagnostics.push_back(diagnostic(
            "PROTECTED_INPUT", "asset", "protected KN5 input cannot be rewritten by this service"));
        return result;
    }
    try {
        auto bake = authoring::buildKn5BakeProject(
            session_->state(), primary_geometry_baselines_, limits_.project_bake);
        const auto baked = formats::bakeKn5(
            *primary_baseline_, bake, limits_.export_limits);
        result.bytes = formats::serializeKn5(baked.model, limits_.export_limits);
        for (const auto& warning : baked.warnings)
            result.diagnostics.push_back(
                diagnostic("BAKE_WARNING", "asset", warning));
        const auto& state = session_->state();
        if (!state.workspaceFiles.empty() || state.workspace.cockpitHrDistance ||
            state.workspace.driverHrDistance) {
            result.diagnostics.push_back(diagnostic(
                "CATEGORY_NOT_IN_KN5", "workspaceEdits",
                "workspace edits require a models.ini or lods.ini export"));
        }
        if (!state.surfaces.empty()) {
            result.diagnostics.push_back(diagnostic(
                "CATEGORY_NOT_IN_KN5", "surfaceEdits",
                "surface edits require a surfaces.ini export"));
        }
        if (!state.colliders.empty() || !state.bottomColliders.empty()) {
            result.diagnostics.push_back(diagnostic(
                "CATEGORY_NOT_IN_KN5", "colliderEdits",
                "collider edits require their secondary asset export"));
        }
        if (!state.damage.empty()) {
            result.diagnostics.push_back(diagnostic(
                "CATEGORY_NOT_IN_KN5", "damageEdits",
                "damage edits require a damage.ini export"));
        }
        result.status = AuthoringServiceStatus::ok;
    } catch (const core::ParseError& error) {
        result.status = AuthoringServiceStatus::invalid;
        result.bytes.clear();
        result.diagnostics.push_back(
            diagnostic(error.code(), error.source(), error.what()));
    } catch (const std::exception& error) {
        result.status = AuthoringServiceStatus::failed;
        result.bytes.clear();
        addException(result, "KN5_EXPORT_FAILED", "asset", error);
    }
    return result;
}

AuthoringServiceOpenResult AuthoringService::openCollider(
    std::string name, std::span<const std::uint8_t> bytes) {
    AuthoringServiceOpenResult result;
    if (!isOpen()) {
        result.status = AuthoringServiceStatus::not_open;
        result.diagnostics.push_back(
            diagnostic("NOT_OPEN", "colliderAsset", "a primary KN5 is not open"));
        return result;
    }
    try {
        auto parseOptions = limits_.primary_parse;
        parseOptions.metadataOnly = false;
        auto model = formats::parseKn5(bytes, name, parseOptions);
        auto identity = secondaryIdentityFor(std::move(name), bytes, model.version);
        auto baseline = authoring::captureProjectColliderBaseline(
            model, limits_.geometry);
        collider_ = ColliderState{std::move(identity), std::move(baseline)};
        result.status = AuthoringServiceStatus::ok;
        result.identity = collider_->identity;
        setRevision(result, session_);
    } catch (const core::ParseError& error) {
        result.status = AuthoringServiceStatus::invalid;
        result.diagnostics.push_back(
            diagnostic(error.code(), error.source(), error.what()));
    } catch (const std::exception& error) {
        result.status = AuthoringServiceStatus::invalid;
        addException(result, "COLLIDER_OPEN_FAILED", "colliderAsset", error);
    }
    return result;
}

AuthoringServiceOpenResult AuthoringService::openDamage(
    std::string name, std::span<const std::uint8_t> bytes) {
    AuthoringServiceOpenResult result;
    if (!isOpen()) {
        result.status = AuthoringServiceStatus::not_open;
        result.diagnostics.push_back(
            diagnostic("NOT_OPEN", "damageAsset", "a primary KN5 is not open"));
        return result;
    }
    try {
        const auto parsed = domain::parse_car_damage_ini(
            textBytes(bytes), name, limits_.damage);
        addDamageDiagnostics(result, parsed.diagnostics, name);
        const bool hasFatalDiagnostic = std::any_of(
            parsed.diagnostics.begin(), parsed.diagnostics.end(),
            [](const auto& item) {
                return item.severity == domain::CarDamageDiagnostic::Severity::error ||
                       item.code == "INI_CONTINUATION";
            });
        if (!parsed.config || parsed.limit_exceeded || hasFatalDiagnostic) {
            result.status = AuthoringServiceStatus::invalid;
            return result;
        }
        const auto identity = secondaryIdentityFor(name, bytes);
        damage_ = DamageState{identity,
                              domain::capture_car_damage_baseline(*parsed.config)};
        result.status = AuthoringServiceStatus::ok;
        result.identity = damage_->identity;
        setRevision(result, session_);
    } catch (const core::ParseError& error) {
        result.status = AuthoringServiceStatus::invalid;
        result.diagnostics.push_back(
            diagnostic(error.code(), error.source(), error.what()));
    } catch (const std::exception& error) {
        result.status = AuthoringServiceStatus::invalid;
        addException(result, "DAMAGE_OPEN_FAILED", "damageAsset", error);
    }
    return result;
}

AuthoringServiceOpenResult AuthoringService::openBottomColliders(
    std::string name, std::span<const std::uint8_t> bytes) {
    AuthoringServiceOpenResult result;
    if (!isOpen()) {
        result.status = AuthoringServiceStatus::not_open;
        result.diagnostics.push_back(diagnostic(
            "NOT_OPEN", "bottomColliderAsset", "a primary KN5 is not open"));
        return result;
    }
    try {
        const auto parsed = domain::parse_bottom_colliders_ini(
            textBytes(bytes), name, limits_.bottom_colliders);
        addBottomDiagnostics(result, parsed.diagnostics, name);
        if (!parsed.config || parsed.limit_exceeded) {
            result.status = AuthoringServiceStatus::invalid;
            return result;
        }
        const auto identity = secondaryIdentityFor(name, bytes);
        bottom_colliders_ = BottomColliderState{
            identity, domain::capture_bottom_collider_baseline(
                          *parsed.config, limits_.bottom_colliders)};
        result.status = AuthoringServiceStatus::ok;
        result.identity = bottom_colliders_->identity;
        setRevision(result, session_);
    } catch (const core::ParseError& error) {
        result.status = AuthoringServiceStatus::invalid;
        result.diagnostics.push_back(
            diagnostic(error.code(), error.source(), error.what()));
    } catch (const std::exception& error) {
        result.status = AuthoringServiceStatus::invalid;
        addException(result, "BOTTOM_COLLIDER_OPEN_FAILED", "bottomColliderAsset", error);
    }
    return result;
}

AuthoringServiceResult AuthoringService::openSurfaces(
    std::string name, std::span<const std::uint8_t> bytes) {
    if (!isOpen()) {
        return failure(AuthoringServiceStatus::not_open, "NOT_OPEN", "surfaceEdits",
                       "a primary KN5 is not open");
    }
    AuthoringServiceResult result;
    auto captured = authoring::captureProjectSurfacesBaseline(
        name, bytes, limits_.surfaces);
    result.status = surfacesStatus(captured.status);
    addSurfaceDiagnostics(result, captured.diagnostics);
    if (captured.ok() && captured.baseline) {
        surfaces_ = std::move(*captured.baseline);
        result.status = AuthoringServiceStatus::ok;
    }
    setRevision(result, session_);
    return result;
}

AuthoringServiceResult AuthoringService::openWorkspace(
    authoring::ProjectWorkspaceKind kind, std::string name,
    std::span<const std::uint8_t> bytes) {
    if (!isOpen()) {
        return failure(AuthoringServiceStatus::not_open, "NOT_OPEN", "workspaceEdits",
                       "a primary KN5 is not open");
    }
    AuthoringServiceResult result;
    auto captured = kind == authoring::ProjectWorkspaceKind::trackModels
                        ? authoring::captureProjectTrackWorkspaceBaseline(
                              name, bytes, limits_.workspace_ini,
                              limits_.workspace)
                        : authoring::captureProjectCarLodWorkspaceBaseline(
                              name, bytes, limits_.workspace_ini,
                              limits_.workspace);
    result.status = workspaceStatus(captured.status);
    addWorkspaceDiagnostics(result, captured.diagnostics);
    if (captured.ok() && captured.baseline) {
        workspace_ = std::move(*captured.baseline);
        result.status = AuthoringServiceStatus::ok;
    }
    setRevision(result, session_);
    return result;
}

AuthoringServiceBytesResult AuthoringService::exportColliderKn5() const {
    AuthoringServiceBytesResult result;
    if (!isOpen()) {
        result.status = AuthoringServiceStatus::not_open;
        result.diagnostics.push_back(
            diagnostic("NOT_OPEN", "colliderAsset", "a primary KN5 is not open"));
        return result;
    }
    if (!collider_) {
        result.status = AuthoringServiceStatus::unbound;
        result.diagnostics.push_back(
            diagnostic("SECONDARY_NOT_OPEN", "colliderAsset", "no collider asset is bound"));
        return result;
    }
    result.suggested_name = "collider.kn5";
    result.revision = session_->state().revision;
    const auto exported = authoring::exportProjectCollider(
        session_->state(), collider_->identity, collider_->baseline,
        limits_.export_limits);
    result.status = secondaryStatus(exported.status);
    result.bytes = exported.bytes;
    addSecondaryDiagnostics(result, exported.diagnostics);
    if (!result.ok()) result.bytes.clear();
    return result;
}

AuthoringServiceTextResult AuthoringService::exportDamageIni() const {
    AuthoringServiceTextResult result;
    if (!isOpen()) {
        result.status = AuthoringServiceStatus::not_open;
        result.diagnostics.push_back(
            diagnostic("NOT_OPEN", "damageAsset", "a primary KN5 is not open"));
        return result;
    }
    if (!damage_) {
        result.status = AuthoringServiceStatus::unbound;
        result.diagnostics.push_back(
            diagnostic("SECONDARY_NOT_OPEN", "damageAsset", "no damage asset is bound"));
        return result;
    }
    result.suggested_name = "damage.ini";
    result.revision = session_->state().revision;
    const auto exported = authoring::exportProjectDamage(
        session_->state(), damage_->identity, damage_->baseline, limits_.damage);
    result.status = secondaryStatus(exported.status);
    result.text = exported.text;
    addSecondaryDiagnostics(result, exported.diagnostics);
    if (!result.ok()) result.text.clear();
    return result;
}

AuthoringServiceTextResult AuthoringService::exportBottomCollidersIni() const {
    AuthoringServiceTextResult result;
    if (!isOpen()) {
        result.status = AuthoringServiceStatus::not_open;
        result.diagnostics.push_back(diagnostic(
            "NOT_OPEN", "bottomColliderAsset", "a primary KN5 is not open"));
        return result;
    }
    if (!bottom_colliders_) {
        result.status = AuthoringServiceStatus::unbound;
        result.diagnostics.push_back(diagnostic(
            "SECONDARY_NOT_OPEN", "bottomColliderAsset", "no colliders.ini asset is bound"));
        return result;
    }
    result.suggested_name = "colliders.ini";
    result.revision = session_->state().revision;
    const auto exported = authoring::exportProjectBottomColliders(
        session_->state(), bottom_colliders_->identity,
        bottom_colliders_->baseline, limits_.bottom_colliders);
    result.status = secondaryStatus(exported.status);
    result.text = exported.text;
    addSecondaryDiagnostics(result, exported.diagnostics);
    if (!result.ok()) result.text.clear();
    return result;
}

AuthoringServiceTextResult AuthoringService::exportSurfacesIni() const {
    AuthoringServiceTextResult result;
    if (!isOpen()) {
        result.status = AuthoringServiceStatus::not_open;
        result.diagnostics.push_back(diagnostic(
            "NOT_OPEN", "surfaceEdits", "a primary KN5 is not open"));
        return result;
    }
    if (!surfaces_) {
        result.status = AuthoringServiceStatus::unbound;
        result.diagnostics.push_back(diagnostic(
            "SECONDARY_NOT_OPEN", "surfaceEdits", "no surfaces.ini asset is bound"));
        return result;
    }
    result.suggested_name = fileName(surfaces_->value().source, "surfaces.ini");
    result.revision = session_->state().revision;
    const auto exported = authoring::exportProjectSurfaces(
        session_->state(), *surfaces_, limits_.surfaces);
    result.status = surfacesStatus(exported.status);
    result.text = exported.text;
    addSurfaceDiagnostics(result, exported.diagnostics);
    if (!result.ok()) result.text.clear();
    return result;
}

AuthoringServiceTextResult AuthoringService::exportWorkspaceIni() const {
    AuthoringServiceTextResult result;
    if (!isOpen()) {
        result.status = AuthoringServiceStatus::not_open;
        result.diagnostics.push_back(diagnostic(
            "NOT_OPEN", "workspaceEdits", "a primary KN5 is not open"));
        return result;
    }
    if (!workspace_) {
        result.status = AuthoringServiceStatus::unbound;
        result.diagnostics.push_back(diagnostic(
            "SECONDARY_NOT_OPEN", "workspaceEdits",
            "no models.ini or lods.ini workspace manifest is bound"));
        return result;
    }
    result.revision = session_->state().revision;
    const auto exported = authoring::exportProjectWorkspace(
        session_->state(), *workspace_, limits_.workspace);
    result.status = workspaceStatus(exported.status);
    result.suggested_name = exported.suggested_name;
    result.text = exported.text;
    addWorkspaceDiagnostics(result, exported.diagnostics);
    if (!result.ok()) result.text.clear();
    return result;
}

bool AuthoringService::isOpen() const noexcept {
    return session_ != nullptr && primary_baseline_.has_value();
}

const ProjectState* AuthoringService::state() const noexcept {
    return session_ ? &session_->state() : nullptr;
}

const SourceIdentity* AuthoringService::primaryIdentity() const noexcept {
    return session_ ? &session_->source() : nullptr;
}

}  // namespace apex::app
