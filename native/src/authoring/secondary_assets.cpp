#include "apex/authoring/secondary_assets.hpp"

#include "apex/authoring/geometry.hpp"
#include "apex/formats/kn5_write.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace apex::authoring {
namespace {

[[nodiscard]] std::optional<SecondaryAssetStatus>
identityFailure(bool hasEdits, const std::optional<SourceIdentity> &expected,
                const std::optional<SourceIdentity> &observed,
                std::string_view path,
                std::vector<SecondaryAssetDiagnostic> &diagnostics) {
  if (!hasEdits)
    return std::nullopt;
  if (!expected || !observed) {
    diagnostics.push_back(
        {"ASSET_IDENTITY_REQUIRED", std::string(path),
         "both project and observed secondary-asset identities are required"});
    return SecondaryAssetStatus::unbound;
  }
  try {
    const auto normalizedExpected = normalizeSecondaryAssetIdentity(*expected);
    const auto normalizedObserved = normalizeSecondaryAssetIdentity(*observed);
    if (!secondaryAssetIdentityMatches(true, normalizedExpected,
                                       normalizedObserved)) {
      diagnostics.push_back(
          {"ASSET_IDENTITY_STALE", std::string(path),
           "the observed secondary asset does not match the project edits"});
      return SecondaryAssetStatus::stale;
    }
  } catch (const AuthoringError &error) {
    diagnostics.push_back({error.code(), std::string(path), error.what()});
    return SecondaryAssetStatus::invalid;
  }
  return std::nullopt;
}

[[nodiscard]] bool canonicalNodePath(std::string_view path) {
  if (path == "root")
    return true;
  if (path.empty())
    return false;
  std::size_t start = 0;
  while (start < path.size()) {
    const auto slash = path.find('/', start);
    const auto part =
        path.substr(start, slash == std::string_view::npos ? path.size() - start
                                                           : slash - start);
    if (part.empty() || (part.size() > 1U && part.front() == '0') ||
        !std::all_of(part.begin(), part.end(), [](char character) {
          return character >= '0' && character <= '9';
        }))
      return false;
    if (slash == std::string_view::npos)
      break;
    start = slash + 1U;
  }
  return true;
}

[[nodiscard]] std::string floatText(float value) {
  if (!std::isfinite(value))
    throw domain::CarDamageError("EDIT_INVALID",
                                 "damage edit number is not finite");
  if (value == 0.0F)
    return "0";
  std::array<char, 64> buffer{};
  const auto result = std::to_chars(
      buffer.data(), buffer.data() + buffer.size(), value,
      std::chars_format::general, std::numeric_limits<float>::max_digits10);
  if (result.ec != std::errc{})
    throw domain::CarDamageError("EDIT_INVALID",
                                 "damage edit number could not be formatted");
  return std::string(buffer.data(), result.ptr);
}

[[nodiscard]] std::string vectorText(const Vector3 &value) {
  return floatText(value[0]) + ", " + floatText(value[1]) + ", " +
         floatText(value[2]);
}

void addDamageFields(domain::CarDamageEdits &output, std::string_view section,
                     const DamageEdit &edit) {
  auto &values = output.values[std::string(section)];
  const auto number = [&](std::string_view name,
                          const std::optional<float> &value) {
    if (value)
      values.emplace(name, floatText(*value));
  };
  const auto vector = [&](std::string_view name,
                          const std::optional<Vector3> &value) {
    if (value)
      values.emplace(name, vectorText(*value));
  };
  number("minSpeed", edit.minSpeed);
  number("maxSpeed", edit.maxSpeed);
  number("initialLevel", edit.initialLevel);
  number("staticRotationAngle", edit.staticRotationAngle);
  number("multG", edit.multG);
  number("fullSpeed", edit.fullSpeed);
  number("oscillationMinAngle", edit.oscillationMinAngle);
  number("oscillationMaxAngle", edit.oscillationMaxAngle);
  vector("staticRotationAxis", edit.staticRotationAxis);
  vector("oscillationAxis", edit.oscillationAxis);
  vector("allowedG", edit.allowedG);
  if (edit.enabled)
    values.emplace("enabled", *edit.enabled ? "1" : "0");
  if (edit.name)
    values.emplace("name", *edit.name);
  if (edit.damageZone)
    values.emplace("damageZone", *edit.damageZone);
}

[[nodiscard]] bool damageEditValid(std::string_view section,
                                   const DamageEdit &edit,
                                   SecondaryAssetDiagnostic &diagnostic) {
  if (!damageSectionValid(section)) {
    diagnostic = {"DAMAGE_SECTION_INVALID",
                  "damageEdits." + std::string(section),
                  "damage section is not modeled"};
    return false;
  }
  const auto check = [&](std::string_view field, bool present) {
    if (!present || damageFieldAllowed(section, field))
      return true;
    diagnostic = {"DAMAGE_FIELD_UNSUPPORTED",
                  "damageEdits." + std::string(section) + "." +
                      std::string(field),
                  "damage field is not valid for its section"};
    return false;
  };
  if (!check("minSpeed", edit.minSpeed.has_value()) ||
      !check("maxSpeed", edit.maxSpeed.has_value()) ||
      !check("initialLevel", edit.initialLevel.has_value()) ||
      !check("staticRotationAngle", edit.staticRotationAngle.has_value()) ||
      !check("multG", edit.multG.has_value()) ||
      !check("fullSpeed", edit.fullSpeed.has_value()) ||
      !check("oscillationMinAngle", edit.oscillationMinAngle.has_value()) ||
      !check("oscillationMaxAngle", edit.oscillationMaxAngle.has_value()) ||
      !check("staticRotationAxis", edit.staticRotationAxis.has_value()) ||
      !check("oscillationAxis", edit.oscillationAxis.has_value()) ||
      !check("allowedG", edit.allowedG.has_value()) ||
      !check("enabled", edit.enabled.has_value()) ||
      !check("name", edit.name.has_value()) ||
      !check("damageZone", edit.damageZone.has_value()))
    return false;
  const bool hasFields = edit.minSpeed || edit.maxSpeed || edit.initialLevel ||
                         edit.staticRotationAngle || edit.multG ||
                         edit.fullSpeed || edit.oscillationMinAngle ||
                         edit.oscillationMaxAngle || edit.staticRotationAxis ||
                         edit.oscillationAxis || edit.allowedG ||
                         edit.enabled || edit.name || edit.damageZone;
  if (!hasFields) {
    diagnostic = {"DAMAGE_EDIT_INVALID", "damageEdits." + std::string(section),
                  "damage edit has no fields"};
    return false;
  }
  const auto invalidVector = [](const std::optional<Vector3> &value) {
    return value &&
           std::any_of(value->begin(), value->end(), [](float component) {
             return !std::isfinite(component);
           });
  };
  if (invalidVector(edit.staticRotationAxis) ||
      invalidVector(edit.oscillationAxis) || invalidVector(edit.allowedG)) {
    diagnostic = {"DAMAGE_EDIT_INVALID", "damageEdits." + std::string(section),
                  "damage edit contains a non-finite vector"};
    return false;
  }
  const auto invalidNumber = [](const std::optional<float> &value) {
    return value && !std::isfinite(*value);
  };
  if (invalidNumber(edit.minSpeed) || invalidNumber(edit.maxSpeed) ||
      invalidNumber(edit.initialLevel) ||
      invalidNumber(edit.staticRotationAngle) || invalidNumber(edit.multG) ||
      invalidNumber(edit.fullSpeed) ||
      invalidNumber(edit.oscillationMinAngle) ||
      invalidNumber(edit.oscillationMaxAngle)) {
    diagnostic = {"DAMAGE_EDIT_INVALID", "damageEdits." + std::string(section),
                  "damage edit contains a non-finite number"};
    return false;
  }
  if ((edit.minSpeed && *edit.minSpeed < 0.0F) ||
      (edit.maxSpeed && *edit.maxSpeed < 0.0F) ||
      (edit.fullSpeed && *edit.fullSpeed < 0.0F) ||
      (edit.initialLevel &&
       (*edit.initialLevel < 0.0F || *edit.initialLevel > 100.0F))) {
    diagnostic = {"DAMAGE_EDIT_INVALID", "damageEdits." + std::string(section),
                  "damage edit contains an out-of-range value"};
    return false;
  }
  if (edit.name) {
    const auto first = edit.name->find_first_not_of(" \t\r\n");
    if (first == std::string::npos || edit.name->size() > 1024U ||
        edit.name->find_first_of("\r\n;") != std::string::npos) {
      diagnostic = {"DAMAGE_EDIT_INVALID",
                    "damageEdits." + std::string(section) + ".name",
                    "damage name contains unsafe text"};
      return false;
    }
  }
  if (edit.damageZone) {
    const bool safe =
        !edit.damageZone->empty() && edit.damageZone->size() <= 64U &&
        std::all_of(edit.damageZone->begin(), edit.damageZone->end(),
                    [](char character) {
                      return (character >= 'A' && character <= 'Z') ||
                             (character >= 'a' && character <= 'z') ||
                             (character >= '0' && character <= '9') ||
                             character == '_' || character == '-';
                    });
    if (!safe) {
      diagnostic = {"DAMAGE_EDIT_INVALID",
                    "damageEdits." + std::string(section) + ".damageZone",
                    "damage zone is not a safe token"};
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::size_t damageFieldCount(const DamageEdit &edit) {
  return static_cast<std::size_t>(edit.minSpeed.has_value()) +
         static_cast<std::size_t>(edit.maxSpeed.has_value()) +
         static_cast<std::size_t>(edit.initialLevel.has_value()) +
         static_cast<std::size_t>(edit.staticRotationAngle.has_value()) +
         static_cast<std::size_t>(edit.multG.has_value()) +
         static_cast<std::size_t>(edit.fullSpeed.has_value()) +
         static_cast<std::size_t>(edit.oscillationMinAngle.has_value()) +
         static_cast<std::size_t>(edit.oscillationMaxAngle.has_value()) +
         static_cast<std::size_t>(edit.staticRotationAxis.has_value()) +
         static_cast<std::size_t>(edit.oscillationAxis.has_value()) +
         static_cast<std::size_t>(edit.allowedG.has_value()) +
         static_cast<std::size_t>(edit.enabled.has_value()) +
         static_cast<std::size_t>(edit.name.has_value()) +
         static_cast<std::size_t>(edit.damageZone.has_value());
}

} // namespace

ColliderAssetBaseline
captureProjectColliderBaseline(const formats::Kn5File &source,
                               GeometryLimits limits) {
  return {source, capture_static_geometry_baselines(source.root, limits)};
}

ColliderAssetResult
applyProjectColliderEdits(const ProjectState &project,
                          const std::optional<SourceIdentity> &observedAsset,
                          const ColliderAssetBaseline &baseline) {
  ColliderAssetResult result;
  if (const auto failure =
          identityFailure(!project.colliders.empty(), project.colliderAsset,
                          observedAsset, "colliderAsset", result.diagnostics)) {
    result.status = *failure;
    return result;
  }
  result.asset = baseline.asset;
  if (project.colliders.empty()) {
    result.status = SecondaryAssetStatus::unchanged;
    return result;
  }
  std::map<std::string, GeometryEdit> edits;
  for (const auto &[path, edit] : project.colliders) {
    if (path.size() > 4096U || !canonicalNodePath(path)) {
      result.asset.reset();
      result.status = SecondaryAssetStatus::invalid;
      result.diagnostics.push_back(
          {"EDIT_INVALID", "colliderEdits." + path,
           "collider path is not a canonical hierarchy path"});
      return result;
    }
    if (!edit.transform && !edit.removeDegenerate && !edit.reverseWinding &&
        !edit.recalculateNormals) {
      result.asset.reset();
      result.status = SecondaryAssetStatus::invalid;
      result.diagnostics.push_back({"EDIT_INVALID", "colliderEdits." + path,
                                    "collider edit has no fields"});
      return result;
    }
    if (edit.removeDegenerate == false || edit.reverseWinding == false ||
        edit.recalculateNormals == false) {
      result.asset.reset();
      result.status = SecondaryAssetStatus::invalid;
      result.diagnostics.push_back(
          {"EDIT_INVALID", "colliderEdits." + path,
           "false collider topology flags are not persisted"});
      return result;
    }
    edits.emplace(path, GeometryEdit{edit.removeDegenerate.value_or(false),
                                     edit.reverseWinding.value_or(false),
                                     edit.recalculateNormals.value_or(false),
                                     edit.transform});
  }
  std::vector<std::string> warnings;
  try {
    result.applied = apply_geometry_edits(result.asset->root, edits,
                                          &baseline.geometry, &warnings);
  } catch (const GeometryError &error) {
    warnings.push_back(error.what());
  }
  if (!warnings.empty() || result.applied != edits.size()) {
    result.asset.reset();
    result.applied = 0;
    result.status = SecondaryAssetStatus::invalid;
    for (const auto &warning : warnings)
      result.diagnostics.push_back(
          {"COLLIDER_EDIT_INVALID", "colliderEdits", warning});
    if (warnings.empty())
      result.diagnostics.push_back(
          {"COLLIDER_EDIT_INVALID", "colliderEdits",
           "collider edits were not applied atomically"});
    return result;
  }
  result.status = SecondaryAssetStatus::applied;
  return result;
}

SecondaryBinaryExportResult
exportProjectCollider(const ProjectState &project,
                      const std::optional<SourceIdentity> &observedAsset,
                      const ColliderAssetBaseline &baseline,
                      core::ParseLimits limits) {
  auto applied = applyProjectColliderEdits(project, observedAsset, baseline);
  SecondaryBinaryExportResult result{
      applied.status, applied.applied, {}, std::move(applied.diagnostics)};
  if (!applied.asset)
    return result;
  try {
    result.bytes = formats::serializeKn5(*applied.asset, limits);
  } catch (const formats::Kn5WriteError &error) {
    result.status = SecondaryAssetStatus::invalid;
    result.applied = 0;
    result.bytes.clear();
    result.diagnostics.push_back({error.code(), "colliderAsset", error.what()});
  }
  return result;
}

DamageAssetResult
applyProjectDamageEdits(const ProjectState &project,
                        const std::optional<SourceIdentity> &observedAsset,
                        const domain::CarDamageBaseline &baseline,
                        domain::CarDamageLimits limits) {
  DamageAssetResult result;
  if (const auto failure =
          identityFailure(!project.damage.empty(), project.damageAsset,
                          observedAsset, "damageAsset", result.diagnostics)) {
    result.status = *failure;
    return result;
  }
  result.asset = baseline.config;
  if (project.damage.empty()) {
    result.status = SecondaryAssetStatus::unchanged;
    return result;
  }
  domain::CarDamageEdits edits;
  try {
    std::size_t fieldCount = 0;
    for (const auto &[section, edit] : project.damage) {
      (void)section;
      const auto count = damageFieldCount(edit);
      if (count > limits.max_extra_entries -
                      std::min(limits.max_extra_entries, fieldCount)) {
        result.asset.reset();
        result.status = SecondaryAssetStatus::invalid;
        result.diagnostics.push_back({"EDIT_LIMIT", "damageEdits",
                                      "damage edit count exceeds its limit"});
        return result;
      }
      fieldCount += count;
    }
    for (const auto &[section, edit] : project.damage) {
      SecondaryAssetDiagnostic diagnostic;
      if (!damageEditValid(section, edit, diagnostic)) {
        result.asset.reset();
        result.status = SecondaryAssetStatus::invalid;
        result.diagnostics.push_back(std::move(diagnostic));
        return result;
      }
      addDamageFields(edits, section, edit);
    }
    result.applied =
        domain::apply_car_damage_edits(*result.asset, edits, baseline, limits);
    result.status = SecondaryAssetStatus::applied;
  } catch (const domain::CarDamageError &error) {
    result.asset.reset();
    result.applied = 0;
    result.status = SecondaryAssetStatus::invalid;
    result.diagnostics.push_back({error.code(), "damageEdits", error.what()});
  }
  return result;
}

SecondaryTextExportResult
exportProjectDamage(const ProjectState &project,
                    const std::optional<SourceIdentity> &observedAsset,
                    const domain::CarDamageBaseline &baseline,
                    domain::CarDamageLimits limits) {
  auto applied =
      applyProjectDamageEdits(project, observedAsset, baseline, limits);
  SecondaryTextExportResult result{
      applied.status, applied.applied, {}, std::move(applied.diagnostics)};
  if (!applied.asset)
    return result;
  try {
    result.text = domain::serialize_car_damage_ini(*applied.asset, limits);
  } catch (const domain::CarDamageError &error) {
    result.status = SecondaryAssetStatus::invalid;
    result.applied = 0;
    result.text.clear();
    result.diagnostics.push_back({error.code(), "damageAsset", error.what()});
  }
  return result;
}

BottomColliderAssetResult applyProjectBottomColliderEdits(
    const ProjectState &project,
    const std::optional<SourceIdentity> &observedAsset,
    const domain::BottomColliderBaseline &baseline,
    domain::BottomColliderLimits limits) {
  BottomColliderAssetResult result;
  if (const auto failure = identityFailure(
          !project.bottomColliders.empty(), project.bottomColliderAsset,
          observedAsset, "bottomColliderAsset", result.diagnostics)) {
    result.status = *failure;
    return result;
  }
  result.asset = baseline.config;
  if (project.bottomColliders.empty()) {
    result.status = SecondaryAssetStatus::unchanged;
    return result;
  }
  domain::BottomColliderEdits edits;
  for (const auto &[index, edit] : project.bottomColliders)
    edits.emplace(index, domain::BottomColliderEdit{edit.centre, edit.size,
                                                    edit.groundEnabled});
  const auto applied = domain::apply_bottom_collider_edits(*result.asset, edits,
                                                           baseline, limits);
  result.applied = applied.applied;
  for (const auto &diagnostic : applied.diagnostics)
    result.diagnostics.push_back(
        {diagnostic.code,
         "bottomColliderEdits." + std::to_string(diagnostic.index),
         diagnostic.message});
  if (!result.diagnostics.empty()) {
    result.asset.reset();
    result.applied = 0;
    result.status = SecondaryAssetStatus::invalid;
  } else {
    result.status = SecondaryAssetStatus::applied;
  }
  return result;
}

SecondaryTextExportResult
exportProjectBottomColliders(const ProjectState &project,
                             const std::optional<SourceIdentity> &observedAsset,
                             const domain::BottomColliderBaseline &baseline,
                             domain::BottomColliderLimits limits) {
  auto applied =
      applyProjectBottomColliderEdits(project, observedAsset, baseline, limits);
  SecondaryTextExportResult result{
      applied.status, applied.applied, {}, std::move(applied.diagnostics)};
  if (!applied.asset)
    return result;
  try {
    result.text =
        domain::serialize_bottom_colliders_ini(*applied.asset, limits);
  } catch (const domain::CarDamageError &error) {
    result.status = SecondaryAssetStatus::invalid;
    result.applied = 0;
    result.text.clear();
    result.diagnostics.push_back(
        {error.code(), "bottomColliderAsset", error.what()});
  }
  return result;
}

} // namespace apex::authoring
