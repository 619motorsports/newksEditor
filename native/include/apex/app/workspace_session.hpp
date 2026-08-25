#pragma once

#include "apex/assets/asset_source.hpp"
#include "apex/formats/ini.hpp"
#include "apex/formats/kn5.hpp"
#include "apex/scene/kn5_scene.hpp"
#include "apex/workspace/workspace.hpp"
#include "apex/workspace/workspace_scene.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::app {

enum class WorkspaceSessionStatus : std::uint8_t {
    ok,
    invalid,
    failed,
};

enum class WorkspaceSessionKind : std::uint8_t {
    generic,
    track,
    carLods,
};

struct WorkspaceSessionDiagnostic {
    std::string code;
    std::string path;
    std::string message;
    std::size_t offset = 0;
};

struct WorkspaceSessionLimits {
    std::size_t maxModels = 100'000;
    std::size_t maxTotalInputBytes = std::size_t{512} * 1024U * 1024U;
    formats::Kn5ParseOptions kn5{};
    formats::IniParseLimits manifestIni{};
    workspace::WorkspaceLimits workspace{};
    scene::Kn5SceneLimits scene{};
    workspace::WorkspaceSceneLimits sceneBinding{};
};

/** A caller-granted model file. The bytes are consumed during open(). */
struct WorkspaceSessionFile {
    std::string name;
    std::span<const std::uint8_t> bytes{};
};

struct WorkspaceSessionOpenRequest {
    WorkspaceSessionKind kind = WorkspaceSessionKind::generic;
    std::string name = "KN5 workspace";
    std::string manifestName;
    std::span<const std::uint8_t> manifestBytes{};
    std::span<const WorkspaceSessionFile> modelFiles{};
};

/** Owned backend-neutral output from one successful workspace open. */
struct WorkspaceSessionDocument {
    workspace::WorkspaceAssembly assembly;
    scene::Kn5SceneConversion scene;
    workspace::WorkspaceSceneBinding sceneBinding;
};

struct WorkspaceSessionResult {
    WorkspaceSessionStatus status = WorkspaceSessionStatus::failed;
    std::vector<WorkspaceSessionDiagnostic> diagnostics;
    std::optional<WorkspaceSessionDocument> document;

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceSessionStatus::ok && document.has_value();
    }
};

/**
 * Bounded application composition for one workspace preview.
 *
 * This class owns no filesystem or graphics capability. Callers may provide
 * already-granted bytes, or an AssetSource may be supplied through
 * openAssetSource(). A failed open returns no document.
 */
class WorkspaceSession final {
public:
    explicit WorkspaceSession(WorkspaceSessionLimits limits = {});

    [[nodiscard]] WorkspaceSessionResult open(
        const WorkspaceSessionOpenRequest& request) const;

    [[nodiscard]] WorkspaceSessionResult openAssetSource(
        WorkspaceSessionKind kind, std::string name, std::string manifestName,
        const assets::AssetSource& source) const;

    [[nodiscard]] const WorkspaceSessionLimits& limits() const noexcept {
        return limits_;
    }

private:
    WorkspaceSessionLimits limits_;
};

[[nodiscard]] const char* workspace_session_status_name(
    WorkspaceSessionStatus status) noexcept;

}  // namespace apex::app
