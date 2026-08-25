#pragma once

#include "apex/authoring/project_bake.hpp"
#include "apex/authoring/project_io.hpp"
#include "apex/authoring/project_surfaces.hpp"
#include "apex/authoring/project_workspace.hpp"
#include "apex/authoring/secondary_assets.hpp"
#include "apex/core/parse_limits.hpp"
#include "apex/domain/car_damage.hpp"
#include "apex/formats/kn5.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::app {

/** Status returned by the in-memory authoring composition boundary. */
enum class AuthoringServiceStatus : std::uint8_t {
    ok,
    not_open,
    invalid,
    stale,
    unbound,
    unsupported,
    failed,
};

struct AuthoringServiceDiagnostic {
    std::string code;
    std::string path;
    std::string message;
    std::size_t line = 0;
};

struct AuthoringServiceResult {
    AuthoringServiceStatus status = AuthoringServiceStatus::failed;
    std::uint64_t revision = 0;
    std::vector<AuthoringServiceDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept {
        return status == AuthoringServiceStatus::ok;
    }
};

struct AuthoringServiceOpenResult : AuthoringServiceResult {
    std::optional<authoring::SourceIdentity> identity;
};

struct AuthoringServiceBytesResult : AuthoringServiceResult {
    std::string suggested_name;
    std::vector<std::uint8_t> bytes;
};

struct AuthoringServiceTextResult : AuthoringServiceResult {
    std::string suggested_name;
    std::string text;
};

struct AuthoringServiceLimits {
    formats::Kn5ParseOptions primary_parse{};
    core::ParseLimits export_limits{};
    authoring::AuthoringLimits project{};
    authoring::ProjectIoLimits project_io{};
    authoring::ProjectBakeLimits project_bake{};
    authoring::GeometryLimits geometry{};
    domain::CarDamageLimits damage{};
    domain::BottomColliderLimits bottom_colliders{};
    domain::TrackDataLimits surfaces{};
    formats::IniParseLimits workspace_ini{};
    workspace::WorkspaceLimits workspace{};
};

/**
 * Owns an authoring session and immutable source baselines without owning a
 * filesystem or renderer capability.
 *
 * Callers supply named byte spans. All outputs are owned byte or text values;
 * this class never opens a path, spawns a process, or passes a path to a
 * renderer. A failed open, project load, transaction, or export leaves the
 * previously committed service state unchanged.
 */
class AuthoringService final {
public:
    explicit AuthoringService(AuthoringServiceLimits limits = {});
    ~AuthoringService();

    AuthoringService(AuthoringService&&) noexcept;
    AuthoringService& operator=(AuthoringService&&) noexcept;
    AuthoringService(const AuthoringService&) = delete;
    AuthoringService& operator=(const AuthoringService&) = delete;

    /** Parse and retain a complete primary KN5 and capture immutable baselines. */
    [[nodiscard]] AuthoringServiceOpenResult openPrimary(
        std::string name, std::span<const std::uint8_t> bytes);

    /**
     * Parse a project for the open primary; any project diagnostic rejects it.
     * Recovery starts a fresh ProjectSession, so the loaded state receives the
     * service-local monotonic revision assigned by that session (currently 1)
     * instead of trusting a serialized revision value.
     */
    [[nodiscard]] AuthoringServiceResult loadProject(std::string_view json);

    [[nodiscard]] AuthoringServiceResult commit(
        const authoring::AuthoringTransaction& transaction);
    [[nodiscard]] AuthoringServiceResult undo();
    [[nodiscard]] AuthoringServiceResult redo();

    [[nodiscard]] AuthoringServiceTextResult saveProject() const;
    [[nodiscard]] AuthoringServiceTextResult exportCsp() const;
    [[nodiscard]] AuthoringServiceBytesResult exportPrimaryKn5() const;

    /** Bind an observed secondary asset from caller-owned bytes. */
    [[nodiscard]] AuthoringServiceOpenResult openCollider(
        std::string name, std::span<const std::uint8_t> bytes);
    [[nodiscard]] AuthoringServiceOpenResult openDamage(
        std::string name, std::span<const std::uint8_t> bytes);
    [[nodiscard]] AuthoringServiceOpenResult openBottomColliders(
        std::string name, std::span<const std::uint8_t> bytes);
    [[nodiscard]] AuthoringServiceResult openSurfaces(
        std::string name, std::span<const std::uint8_t> bytes);
    [[nodiscard]] AuthoringServiceResult openWorkspace(
        authoring::ProjectWorkspaceKind kind, std::string name,
        std::span<const std::uint8_t> bytes);

    [[nodiscard]] AuthoringServiceBytesResult exportColliderKn5() const;
    [[nodiscard]] AuthoringServiceTextResult exportDamageIni() const;
    [[nodiscard]] AuthoringServiceTextResult exportBottomCollidersIni() const;
    [[nodiscard]] AuthoringServiceTextResult exportSurfacesIni() const;
    [[nodiscard]] AuthoringServiceTextResult exportWorkspaceIni() const;

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const authoring::ProjectState* state() const noexcept;
    [[nodiscard]] const authoring::SourceIdentity* primaryIdentity() const noexcept;
    [[nodiscard]] const AuthoringServiceLimits& limits() const noexcept {
        return limits_;
    }

private:
    struct ColliderState {
        authoring::SourceIdentity identity;
        authoring::ColliderAssetBaseline baseline;
    };
    struct DamageState {
        authoring::SourceIdentity identity;
        domain::CarDamageBaseline baseline;
    };
    struct BottomColliderState {
        authoring::SourceIdentity identity;
        domain::BottomColliderBaseline baseline;
    };

    AuthoringServiceLimits limits_;
    std::unique_ptr<authoring::ProjectSession> session_;
    std::optional<formats::Kn5File> primary_baseline_;
    authoring::GeometryBaselines primary_geometry_baselines_;
    std::optional<ColliderState> collider_;
    std::optional<DamageState> damage_;
    std::optional<BottomColliderState> bottom_colliders_;
    std::optional<authoring::ProjectSurfacesBaseline> surfaces_;
    std::optional<authoring::ProjectWorkspaceBaseline> workspace_;
};

[[nodiscard]] const char* authoring_service_status_name(
    AuthoringServiceStatus status) noexcept;

}  // namespace apex::app
