#pragma once

#include "apex/authoring/project.hpp"
#include "apex/core/parse_limits.hpp"
#include "apex/domain/car_damage.hpp"
#include "apex/formats/kn5.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace apex::authoring {

enum class SecondaryAssetStatus : std::uint8_t {
  unchanged,
  applied,
  unbound,
  stale,
  invalid,
};

struct SecondaryAssetDiagnostic {
  std::string code;
  std::string path;
  std::string message;
};

struct ColliderAssetResult {
  SecondaryAssetStatus status = SecondaryAssetStatus::invalid;
  std::optional<formats::Kn5File> asset;
  std::size_t applied = 0;
  std::vector<SecondaryAssetDiagnostic> diagnostics;
};

struct ColliderAssetBaseline {
  formats::Kn5File asset;
  GeometryBaselines geometry;
};

struct DamageAssetResult {
  SecondaryAssetStatus status = SecondaryAssetStatus::invalid;
  std::optional<domain::CarDamageConfig> asset;
  std::size_t applied = 0;
  std::vector<SecondaryAssetDiagnostic> diagnostics;
};

struct BottomColliderAssetResult {
  SecondaryAssetStatus status = SecondaryAssetStatus::invalid;
  std::optional<domain::BottomColliderConfig> asset;
  std::size_t applied = 0;
  std::vector<SecondaryAssetDiagnostic> diagnostics;
};

struct SecondaryBinaryExportResult {
  SecondaryAssetStatus status = SecondaryAssetStatus::invalid;
  std::size_t applied = 0;
  std::vector<std::uint8_t> bytes;
  std::vector<SecondaryAssetDiagnostic> diagnostics;
};

struct SecondaryTextExportResult {
  SecondaryAssetStatus status = SecondaryAssetStatus::invalid;
  std::size_t applied = 0;
  std::string text;
  std::vector<SecondaryAssetDiagnostic> diagnostics;
};

[[nodiscard]] ColliderAssetBaseline
captureProjectColliderBaseline(const formats::Kn5File &source,
                               GeometryLimits limits = {});
[[nodiscard]] ColliderAssetResult
applyProjectColliderEdits(const ProjectState &project,
                          const std::optional<SourceIdentity> &observedAsset,
                          const ColliderAssetBaseline &baseline);
[[nodiscard]] SecondaryBinaryExportResult
exportProjectCollider(const ProjectState &project,
                      const std::optional<SourceIdentity> &observedAsset,
                      const ColliderAssetBaseline &baseline,
                      core::ParseLimits limits = {});

[[nodiscard]] DamageAssetResult
applyProjectDamageEdits(const ProjectState &project,
                        const std::optional<SourceIdentity> &observedAsset,
                        const domain::CarDamageBaseline &baseline,
                        domain::CarDamageLimits limits = {});
[[nodiscard]] SecondaryTextExportResult
exportProjectDamage(const ProjectState &project,
                    const std::optional<SourceIdentity> &observedAsset,
                    const domain::CarDamageBaseline &baseline,
                    domain::CarDamageLimits limits = {});

[[nodiscard]] BottomColliderAssetResult applyProjectBottomColliderEdits(
    const ProjectState &project,
    const std::optional<SourceIdentity> &observedAsset,
    const domain::BottomColliderBaseline &baseline,
    domain::BottomColliderLimits limits = {});
[[nodiscard]] SecondaryTextExportResult
exportProjectBottomColliders(const ProjectState &project,
                             const std::optional<SourceIdentity> &observedAsset,
                             const domain::BottomColliderBaseline &baseline,
                             domain::BottomColliderLimits limits = {});

} // namespace apex::authoring
