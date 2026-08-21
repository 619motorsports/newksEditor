#pragma once

#include "apex/core/parse_error.hpp"
#include "apex/formats/kn5.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace apex::authoring {

using AuthoringError = apex::core::ParseError;
using Matrix4 = apex::formats::Kn5Matrix4;
using Vector3 = std::array<float, 3>;

struct SourceIdentity {
    std::string name;
    std::uint64_t size = 0;
    std::string sha256;
    std::optional<std::uint32_t> kn5Version;
};

[[nodiscard]] SourceIdentity normalizeSourceIdentity(SourceIdentity identity);
[[nodiscard]] bool sourceIdentityMatches(const SourceIdentity& expected,
                                         const SourceIdentity& actual);

struct AuthoringLimits {
    std::size_t maxHistory = 128;
    std::size_t maxOperationsPerTransaction = 512;
    std::size_t maxTotalEdits = 10'000;
    std::size_t maxStringBytes = 4096;
    std::size_t maxRecoveryEdits = 10'000;
};

struct NodeEdit {
    std::optional<std::string> name;
    std::optional<bool> active;
    std::optional<Matrix4> transform;
};

struct WorkspaceFileEdit {
    std::optional<std::string> name;
    std::optional<Vector3> position;
    std::optional<Vector3> rotation;
    std::optional<float> lodIn;
    std::optional<float> lodOut;
    std::optional<float> probability;
    std::optional<std::array<float, 2>> multiplicity;
    std::optional<std::string> posMode;
    std::optional<Vector3> positionCenter;
    std::optional<Vector3> positionRange;
    std::optional<std::string> velMode;
    std::optional<Vector3> velocityBase;
    std::optional<Vector3> velocityRange;
    std::optional<std::string> playWav;
};

struct WorkspaceSettingsEdit {
    std::optional<float> cockpitHrDistance;
    std::optional<float> driverHrDistance;
};

using MaterialScalar = std::variant<float, bool, std::string>;

struct MaterialVector {
    std::array<float, 4> values{};
    std::uint8_t components = 0;
};

struct MaterialResource {
    bool clear = false;
    std::optional<std::string> texture;
    std::optional<std::string> file;
    std::optional<std::array<float, 4>> color;
};

struct MaterialEdit {
    std::map<std::string, MaterialScalar> scalars;
    std::map<std::string, MaterialVector> vectors;
    std::map<std::string, MaterialResource> resources;
};

struct SurfaceEdit {
    std::optional<std::string> key;
    std::optional<float> friction;
    std::optional<float> damping;
    std::optional<float> dirtAdditive;
    std::optional<float> blackFlagTime;
    std::optional<bool> isValidTrack;
    std::optional<bool> isPitlane;
    std::optional<float> sinHeight;
    std::optional<float> sinLength;
    std::optional<float> vibrationGain;
    std::optional<float> vibrationLength;
    std::optional<std::string> wav;
    std::optional<float> wavPitch;
    std::optional<std::string> ffEffect;
};

struct ColliderEdit {
    std::optional<Matrix4> transform;
    std::optional<bool> removeDegenerate;
    std::optional<bool> reverseWinding;
    std::optional<bool> recalculateNormals;
};

struct BottomColliderEdit {
    std::optional<Vector3> centre;
    std::optional<Vector3> size;
    std::optional<bool> groundEnabled;
};

struct DamageEdit {
    std::optional<float> minSpeed;
    std::optional<float> maxSpeed;
    std::optional<float> initialLevel;
    std::optional<float> staticRotationAngle;
    std::optional<float> multG;
    std::optional<float> fullSpeed;
    std::optional<float> oscillationMinAngle;
    std::optional<float> oscillationMaxAngle;
    std::optional<Vector3> staticRotationAxis;
    std::optional<Vector3> oscillationAxis;
    std::optional<Vector3> allowedG;
    std::optional<bool> enabled;
    std::optional<std::string> name;
    std::optional<std::string> damageZone;
};

struct SetNodeEdit { std::string path; NodeEdit edit; };
struct ClearNodeEdit { std::string path; };
struct SetWorkspaceFileEdit { std::uint32_t index = 0; WorkspaceFileEdit edit; };
struct ClearWorkspaceFileEdit { std::uint32_t index = 0; };
struct SetWorkspaceSettingsEdit { WorkspaceSettingsEdit edit; };
struct ClearWorkspaceSettingsEdit {};
struct SetMaterialScalarEdit { std::string material; std::string field; MaterialScalar value; };
struct SetMaterialVectorEdit { std::string material; std::string field; MaterialVector value; };
struct SetMaterialResourceEdit { std::string material; std::string slot; MaterialResource value; };
struct ClearMaterialEdit { std::string material; };
struct SetSurfaceEdit { std::uint32_t index = 0; SurfaceEdit edit; };
struct ClearSurfaceEdit { std::uint32_t index = 0; };
struct SetColliderEdit { std::uint32_t index = 0; ColliderEdit edit; };
struct ClearColliderEdit { std::uint32_t index = 0; };
struct SetBottomColliderEdit { std::uint32_t index = 0; BottomColliderEdit edit; };
struct ClearBottomColliderEdit { std::uint32_t index = 0; };
struct SetDamageEdit { std::string section; DamageEdit edit; };
struct ClearDamageEdit { std::string section; };

using EditOperation = std::variant<
    SetNodeEdit, ClearNodeEdit,
    SetWorkspaceFileEdit, ClearWorkspaceFileEdit,
    SetWorkspaceSettingsEdit, ClearWorkspaceSettingsEdit,
    SetMaterialScalarEdit, SetMaterialVectorEdit, SetMaterialResourceEdit, ClearMaterialEdit,
    SetSurfaceEdit, ClearSurfaceEdit,
    SetColliderEdit, ClearColliderEdit,
    SetBottomColliderEdit, ClearBottomColliderEdit,
    SetDamageEdit, ClearDamageEdit>;

struct AuthoringTransaction {
    std::string label;
    std::vector<EditOperation> operations;
};

struct ProjectState {
    SourceIdentity source;
    std::uint64_t revision = 0;
    WorkspaceSettingsEdit workspace;
    std::map<std::uint32_t, WorkspaceFileEdit> workspaceFiles;
    std::map<std::string, MaterialEdit> materials;
    std::map<std::string, NodeEdit> nodes;
    std::map<std::uint32_t, SurfaceEdit> surfaces;
    std::map<std::uint32_t, ColliderEdit> colliders;
    std::map<std::uint32_t, BottomColliderEdit> bottomColliders;
    std::map<std::string, DamageEdit> damage;
};

struct RecoverySnapshot {
    ProjectState state;
};

struct RecoveryDiagnostic {
    std::string code;
    std::string message;
};

struct RecoveryResult {
    bool restored = false;
    std::vector<RecoveryDiagnostic> diagnostics;
};

class ProjectSession final {
public:
    explicit ProjectSession(SourceIdentity source, AuthoringLimits limits = {});

    [[nodiscard]] const SourceIdentity& source() const noexcept { return source_; }
    [[nodiscard]] const ProjectState& state() const noexcept { return current_; }
    [[nodiscard]] ProjectState baselineState() const;
    [[nodiscard]] const AuthoringLimits& limits() const noexcept { return limits_; }
    [[nodiscard]] bool canUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::size_t undoCount() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t redoCount() const noexcept { return redo_.size(); }

    // Applies a complete transaction atomically. Invalid operations leave state and history unchanged.
    std::uint64_t commit(const AuthoringTransaction& transaction);
    std::uint64_t undo();
    std::uint64_t redo();
    std::uint64_t restoreBaseline();

    [[nodiscard]] RecoverySnapshot recoverySnapshot() const;
    [[nodiscard]] RecoveryResult validateRecovery(const RecoverySnapshot& snapshot,
                                                  const SourceIdentity& observedSource) const;
    RecoveryResult recover(const RecoverySnapshot& snapshot,
                           const SourceIdentity& observedSource);

private:
    [[nodiscard]] ProjectState applyTransaction(const AuthoringTransaction& transaction) const;
    [[nodiscard]] std::size_t editCount(const ProjectState& state) const;
    [[nodiscard]] RecoveryResult validateRecoveryInternal(const RecoverySnapshot& snapshot,
                                                          const SourceIdentity& observedSource) const;
    void pushUndo(const ProjectState& state);

    SourceIdentity source_;
    AuthoringLimits limits_;
    ProjectState current_;
    std::deque<ProjectState> undo_;
    std::deque<ProjectState> redo_;
};

using AuthoringProject = ProjectSession;

} // namespace apex::authoring
