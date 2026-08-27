#include "apex/authoring/secondary_assets.hpp"
#include "apex/formats/kn5.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace apex::authoring;
using namespace apex::domain;
using namespace apex::formats;

void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}

SourceIdentity identity(char hash = 'a') {
  return {"collider.kn5", 128, std::string(64, hash), 6};
}

Kn5Node mesh(std::string name) {
  Kn5Node node;
  node.type = 2U;
  node.kind = "mesh";
  node.name = std::move(name);
  node.active = true;
  node.visible = true;
  node.renderable = true;
  node.vertexStride = 11U;
  node.vertices.assign(33U, 0.0F);
  node.vertices[11] = 2.0F;
  node.vertices[23] = 2.0F;
  for (std::size_t offset = 0; offset < node.vertices.size(); offset += 11U) {
    node.vertices[offset + 5U] = 1.0F;
    node.vertices[offset + 8U] = 1.0F;
  }
  node.indices = {0, 1, 2};
  node.bounds = {1, 1, 0, std::sqrt(2.0F)};
  return node;
}

Kn5File collider() {
  Kn5File file;
  file.version = 6U;
  Kn5Material material;
  material.name = "GL";
  material.shader = "GL";
  file.materials.push_back(std::move(material));
  file.root.type = 1U;
  file.root.kind = "node";
  file.root.name = "root";
  file.root.active = true;
  file.root.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  Kn5Node group;
  group.type = 1U;
  group.kind = "node";
  group.name = "group";
  group.active = true;
  group.transform = file.root.transform;
  group.children.push_back(mesh("nested"));
  file.root.children.push_back(std::move(group));
  file.root.children.push_back(mesh("direct"));
  return file;
}

void colliderPathsApplyAndExportAtomically() {
  const auto source = collider();
  const auto baseline = captureProjectColliderBaseline(source);
  const auto beforeNested = source.root.children[0].children[0].indices;
  const auto beforeDirect = source.root.children[1].vertices;
  ProjectState project;
  project.colliderAsset = identity();
  ColliderEdit nested;
  nested.reverseWinding = true;
  ColliderEdit direct;
  direct.transform = Matrix4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 0, 0, 1};
  project.colliders.emplace("0/0", nested);
  project.colliders.emplace("1", direct);

  const auto applied = applyProjectColliderEdits(project, identity(), baseline);
  require(applied.status == SecondaryAssetStatus::applied &&
              applied.applied == 2U && applied.asset.has_value() &&
              applied.diagnostics.empty(),
          "matching collider identity applies stable nested paths");
  require(applied.asset->root.children[0].children[0].indices ==
                  std::vector<std::uint16_t>({0, 2, 1}) &&
              applied.asset->root.children[1].vertices[0] == 3.0F,
          "collider geometry operations reach their declared paths");
  require(source.root.children[0].children[0].indices == beforeNested &&
              source.root.children[1].vertices == beforeDirect,
          "collider adapter does not mutate its source");
  const auto reapplied =
      applyProjectColliderEdits(project, identity(), baseline);
  require(
      reapplied.asset && applied.asset &&
          reapplied.asset->root.children[1].vertices ==
              applied.asset->root.children[1].vertices,
      "collider edits reapply deterministically from the immutable baseline");

  const auto exported = exportProjectCollider(project, identity(), baseline);
  require(exported.status == SecondaryAssetStatus::applied &&
              !exported.bytes.empty(),
          "matching collider edits export a KN5");
  const auto reparsed = parseKn5(exported.bytes);
  require(reparsed.root.children[0].children[0].indices ==
              std::vector<std::uint16_t>({0, 2, 1}),
          "exported collider KN5 retains the authored topology");

  project.colliders.emplace("9", nested);
  const auto failed = applyProjectColliderEdits(project, identity(), baseline);
  require(
      failed.status == SecondaryAssetStatus::invalid && !failed.asset &&
          failed.applied == 0U &&
          source.root.children[0].children[0].indices == beforeNested,
      "one missing collider path rejects the full edit set without mutation");
}

void colliderIdentityGateFailsClosed() {
  const auto source = collider();
  const auto baseline = captureProjectColliderBaseline(source);
  ProjectState project;
  ColliderEdit edit;
  edit.reverseWinding = true;
  project.colliders.emplace("0/0", edit);
  project.colliderAsset = identity();
  auto stale = identity('b');
  const auto staleResult = applyProjectColliderEdits(project, stale, baseline);
  require(staleResult.status == SecondaryAssetStatus::stale &&
              !staleResult.asset &&
              staleResult.diagnostics.front().code == "ASSET_IDENTITY_STALE",
          "stale collider hashes fail closed");
  const auto missingResult =
      applyProjectColliderEdits(project, std::nullopt, baseline);
  require(missingResult.status == SecondaryAssetStatus::unbound &&
              !missingResult.asset,
          "missing observed collider identity fails closed");
  project.colliderAsset->sha256 = "short";
  const auto malformed =
      applyProjectColliderEdits(project, identity(), baseline);
  require(
      malformed.status == SecondaryAssetStatus::invalid && !malformed.asset &&
          malformed.diagnostics.front().code == "SOURCE_IDENTITY_INVALID",
      "malformed project collider identity is distinguished from stale input");

  project.colliderAsset = identity();
  project.colliders.clear();
  project.colliders.emplace("0/0", ColliderEdit{});
  const auto empty = applyProjectColliderEdits(project, identity(), baseline);
  require(empty.status == SecondaryAssetStatus::invalid && !empty.asset,
          "empty direct collider edit is rejected");
}

CarDamageBaseline damageBaseline() {
  CarDamageConfig config;
  config.scratches.min_speed = 0.0F;
  config.scratches.max_speed = 20.0F;
  VisualObject visual;
  visual.index = 0U;
  visual.object_name = "HOOD";
  visual.min_speed = 2.0F;
  visual.full_speed = 10.0F;
  config.visual_objects.push_back(std::move(visual));
  return capture_car_damage_baseline(config);
}

void damageApplyAndExportUseImmutableBaseline() {
  const auto baseline = damageBaseline();
  ProjectState project;
  auto damageIdentity = identity();
  damageIdentity.name = "data/damage.ini";
  damageIdentity.kn5Version.reset();
  project.damageAsset = damageIdentity;
  DamageEdit scratches;
  scratches.minSpeed = 5.0F;
  scratches.maxSpeed = 30.0F;
  DamageEdit oscillations;
  oscillations.enabled = false;
  DamageEdit defaults;
  defaults.initialLevel = 25.0F;
  DamageEdit visual;
  visual.name = "HOOD_DAMAGED";
  visual.damageZone = "front-left";
  visual.staticRotationAxis = apex::authoring::Vector3{1, 0, 0};
  visual.staticRotationAngle = 2.0F;
  visual.multG = 3.0F;
  visual.minSpeed = 4.0F;
  visual.fullSpeed = 12.0F;
  visual.oscillationAxis = apex::authoring::Vector3{0, 1, 0};
  visual.oscillationMinAngle = -2.0F;
  visual.oscillationMaxAngle = 5.0F;
  visual.allowedG = apex::authoring::Vector3{1, 2, 3};
  project.damage.emplace("SCRATCHES", scratches);
  project.damage.emplace("OSCILLATIONS", oscillations);
  project.damage.emplace("DAMAGE", defaults);
  project.damage.emplace("VISUAL_OBJECT_0", visual);

  const auto first = applyProjectDamageEdits(project, damageIdentity, baseline);
  const auto second = exportProjectDamage(project, damageIdentity, baseline);
  const auto repeated = exportProjectDamage(project, damageIdentity, baseline);
  require(first.status == SecondaryAssetStatus::applied &&
              first.applied == 15U && first.asset &&
              first.asset->scratches.min_speed == 5.0F &&
              !first.asset->oscillations.enabled &&
              first.asset->damage.initial_level == 25.0F &&
              first.asset->visual_objects.front().object_name == "HOOD_DAMAGED",
          "typed damage edits apply through the domain baseline");
  require(second.status == SecondaryAssetStatus::applied &&
              second.text.find("NAME=HOOD_DAMAGED") != std::string::npos &&
              second.text.find("DAMAGE_ZONE=FRONT-LEFT") != std::string::npos,
          "damage adapter exports deterministic INI text");
  require(repeated.text == second.text,
          "damage export repeats exactly from the immutable baseline");
  require(baseline.config.scratches.min_speed == 0.0F &&
              baseline.config.oscillations.enabled &&
              baseline.config.visual_objects.front().object_name == "HOOD",
          "damage apply and export leave the baseline unchanged");

  ProjectState malformed = project;
  malformed.damage.clear();
  DamageEdit wrong;
  wrong.initialLevel = 25.0F;
  malformed.damage.emplace("VISUAL_OBJECT_X", wrong);
  const auto rejected =
      applyProjectDamageEdits(malformed, damageIdentity, baseline);
  require(rejected.status == SecondaryAssetStatus::invalid && !rejected.asset &&
              rejected.diagnostics.front().code == "DAMAGE_SECTION_INVALID",
          "malformed damage sections are diagnosed without mutation");

  auto staleIdentity = damageIdentity;
  staleIdentity.sha256 = std::string(64, 'b');
  const auto stale = applyProjectDamageEdits(project, staleIdentity, baseline);
  const auto missing = applyProjectDamageEdits(project, std::nullopt, baseline);
  require(stale.status == SecondaryAssetStatus::stale && !stale.asset &&
              missing.status == SecondaryAssetStatus::unbound && !missing.asset,
          "stale and missing damage identities fail closed");
  auto malformedIdentity = damageIdentity;
  malformedIdentity.sha256 = "short";
  project.damageAsset = malformedIdentity;
  const auto malformedIdentityResult =
      applyProjectDamageEdits(project, damageIdentity, baseline);
  require(malformedIdentityResult.status == SecondaryAssetStatus::invalid &&
              !malformedIdentityResult.asset &&
              malformedIdentityResult.diagnostics.front().code ==
                  "SOURCE_IDENTITY_INVALID",
          "malformed damage identity fails closed");
}

void bottomColliderEditsArePositionalAndKeepSourceIds() {
  BottomColliderConfig config;
  BottomCollider box;
  box.centre = {0, 0, 0};
  box.size = {1, 2, 3};
  box.source_index = 2U;
  config.colliders.push_back(box);
  const auto baseline = capture_bottom_collider_baseline(config);
  ProjectState project;
  auto bottomIdentity = identity();
  bottomIdentity.name = "data/colliders.ini";
  bottomIdentity.kn5Version.reset();
  project.bottomColliderAsset = bottomIdentity;
  apex::authoring::BottomColliderEdit edit;
  edit.centre = apex::authoring::Vector3{4, 5, 6};
  edit.groundEnabled = false;
  project.bottomColliders.emplace(0U, edit);
  const auto applied =
      applyProjectBottomColliderEdits(project, bottomIdentity, baseline);
  const auto exported =
      exportProjectBottomColliders(project, bottomIdentity, baseline);
  require(applied.status == SecondaryAssetStatus::applied &&
              applied.applied == 2U && applied.asset &&
              applied.asset->colliders.front().centre[0] == 4.0F &&
              !applied.asset->colliders.front().ground_enabled,
          "bottom-collider edit key addresses the parsed list position");
  require(exported.status == SecondaryAssetStatus::applied &&
              exported.text.find("[COLLIDER_2]") != std::string::npos &&
              exported.text.find("CENTRE=4, 5, 6") != std::string::npos,
          "bottom-collider export retains a sparse source section id");
  require(baseline.config.colliders.front().centre[0] == 0.0F,
          "bottom-collider export leaves its baseline unchanged");

  auto staleIdentity = bottomIdentity;
  staleIdentity.size += 1U;
  const auto stale =
      applyProjectBottomColliderEdits(project, staleIdentity, baseline);
  const auto missing =
      applyProjectBottomColliderEdits(project, std::nullopt, baseline);
  require(stale.status == SecondaryAssetStatus::stale && !stale.asset &&
              missing.status == SecondaryAssetStatus::unbound && !missing.asset,
          "stale and missing bottom-collider identities fail closed");
  auto malformedIdentity = bottomIdentity;
  malformedIdentity.sha256 = "short";
  project.bottomColliderAsset = malformedIdentity;
  const auto malformed =
      applyProjectBottomColliderEdits(project, bottomIdentity, baseline);
  require(malformed.status == SecondaryAssetStatus::invalid &&
              !malformed.asset &&
              malformed.diagnostics.front().code == "SOURCE_IDENTITY_INVALID",
          "malformed bottom-collider identity fails closed");
  project.bottomColliderAsset = bottomIdentity;

  project.bottomColliders[0U].size = apex::authoring::Vector3{1, 0, 1};
  const auto invalid =
      exportProjectBottomColliders(project, bottomIdentity, baseline);
  require(invalid.status == SecondaryAssetStatus::invalid &&
              invalid.text.empty() &&
              baseline.config.colliders.front().size[1] == 2.0F,
          "invalid bottom-collider size cannot partially apply or export");
  auto rejectedBaseline = baseline;
  rejectedBaseline.config.rejected_sections = 1U;
  const auto rejected = exportProjectBottomColliders(
      ProjectState{}, std::nullopt, rejectedBaseline);
  require(rejected.status == SecondaryAssetStatus::invalid &&
              rejected.text.empty() &&
              rejected.diagnostics.front().code == "COLLIDER_REJECTED",
          "rejected source sections block bottom-collider export");
}

} // namespace

int main() {
  try {
    colliderPathsApplyAndExportAtomically();
    colliderIdentityGateFailsClosed();
    damageApplyAndExportUseImmutableBaseline();
    bottomColliderEditsArePositionalAndKeepSourceIds();
    std::cout << "secondary_asset_tests: ok\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "secondary_asset_tests: " << error.what() << '\n';
    return 1;
  }
}
