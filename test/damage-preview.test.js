import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { assettoPath } from "./fixture-paths.js";
import { parseKn5 } from "../src/kn5.js";
import { auditNativeDamagePreview, damagePreviewBranchActive, nativeDamageGlassMaterials, nativeDamageGlassValue, nativeDamageRootForPath, nativeDamageToggle, stockDamageAmount, stockDamageResponse } from "../src/damage-preview.js";

function node(name, active = true, children = []) { return { name, active, children }; }
function material(name, shader = "ksPerPixelMultiMap_damage_dirt", resources = ["txDamage", "txDamageMask"]) {
  return { name, shader, properties: [{ name: "damageZones", value4: [0, 0, 0, 0] }, { name: "dirt", value: 0 }], resources: resources.map((slot) => ({ slot })) };
}

test("uses the five native prefix sequences and stops at the first gap", () => {
  const front1 = node("DAMAGE_GLASS_FRONT_1", false), front3 = node("DAMAGE_GLASS_FRONT_3", false), rear1 = node("DAMAGE_GLASS_REAR_1", false);
  const audit = auditNativeDamagePreview(node("ROOT", true, [front3, node("damage_glass_front_1"), front1, rear1, node("DAMAGE_GLASS_REAR_01")]), []);
  assert.deepEqual(audit.groups.map((group) => group.selected.map((entry) => entry.name)), [["DAMAGE_GLASS_FRONT_1"], ["DAMAGE_GLASS_REAR_1"], [], [], []]);
  assert.deepEqual(audit.groups[0].ignored, [front3]);
  assert.ok(audit.warnings.some((warning) => warning.includes("DAMAGE_GLASS_FRONT_2 is missing")));
});

test("uses the first duplicate node and toggles on for the first F4 edge", () => {
  const first = node("DAMAGE_GLASS_FRONT_1", false), second = node("DAMAGE_GLASS_FRONT_1", false);
  const audit = auditNativeDamagePreview(node("ROOT", true, [first, second]), []);
  assert.equal(audit.roots[0], first);
  assert.equal(nativeDamageToggle(audit), true);
  assert.equal(nativeDamageToggle(audit, true), false);
  assert.equal(nativeDamageToggle(audit, false), true);
  assert.ok(audit.warnings.some((warning) => warning.includes("first match")));
});

test("overrides only native-selected roots and keeps other inactive ancestors", () => {
  const root = node("ROOT"), damage = node("DAMAGE_GLASS_FRONT_1", false), mesh = node("MESH"), ignored = node("DAMAGE_GLASS_FRONT_3", false);
  const audit = auditNativeDamagePreview(node("SCENE", true, [damage, ignored]), []);
  assert.equal(damagePreviewBranchActive([root, damage, mesh], audit, true), true);
  assert.equal(damagePreviewBranchActive([root, damage, mesh], audit, false), false);
  assert.equal(damagePreviewBranchActive([root, ignored, mesh], audit, true), false);
  assert.equal(damagePreviewBranchActive([node("INACTIVE", false), damage, mesh], audit, true), false);
  assert.equal(nativeDamageRootForPath([root, damage, mesh], audit), damage);
});

test("sets glassDamage through shared material identity after either F4 state", () => {
  const damage = node("DAMAGE_GLASS_FRONT_1", false), mesh = node("MESH"), shared = material("GLASS");
  shared.properties.push({ name: "glassDamage", value: 0.25 });
  const audit = auditNativeDamagePreview(node("SCENE", true, [damage]), []);
  const affected = nativeDamageGlassMaterials([
    { ancestors: [damage, mesh], material: shared },
    { ancestors: [node("OTHER"), mesh], material: shared }
  ], audit);
  assert.equal(nativeDamageGlassValue(shared, affected, null, 0.25), 0.25);
  assert.equal(nativeDamageGlassValue(shared, affected, true, 0.25), 1);
  assert.equal(nativeDamageGlassValue(shared, affected, false, 0.25), 1);
  assert.equal(nativeDamageGlassValue(material("OTHER"), affected, true, 0.25), 0.25);
});

test("does not write a shared material that has no exact glassDamage variable", () => {
  const damage = node("DAMAGE_GLASS_FRONT_1", false), withoutGlassDamage = material("BODY");
  const audit = auditNativeDamagePreview(node("SCENE", true, [damage]), []);
  const affected = nativeDamageGlassMaterials([{ ancestors: [damage], material: withoutGlassDamage }], audit);
  assert.equal(affected.size, 0);
});

test("labels only the recovered dirt-zero stock branch as exact", () => {
  const exact = material("CLEAN"), unsupported = material("DIRTY");
  unsupported.properties.find((entry) => entry.name === "dirt").value = 0.4;
  const audit = auditNativeDamagePreview(node("ROOT"), [exact, unsupported]);
  assert.deepEqual(audit.exactMaterials.map((entry) => entry.material.name), ["CLEAN"]);
  assert.equal(audit.materialEntries[1].exactZeroDirt, false);
  assert.ok(audit.warnings.some((warning) => warning.includes("dirt-zero branch")));
});

test("reproduces the stock damage-mask dot, diffuse mix, and specular reduction", () => {
  assert.equal(stockDamageAmount(.5, [.2, .4, .6, .8], [1, 0, .5, 0]), .25);
  assert.equal(stockDamageAmount(2, [1, 1, 1, 1], [1, 1, 1, 1]), 1);
  const response = stockDamageResponse([.2, .4, .6], [1, 0, .5], .8, .5, .25);
  assert.deepEqual(response.diffuse, [.4, .30000000000000004, .575]);
  assert.ok(Math.abs(response.specularMap - .525) < 1e-12);
});

test("diagnoses an incomplete stock damage material", () => {
  const audit = auditNativeDamagePreview(node("ROOT"), [material("BODY", undefined, ["txDamage"])]);
  assert.equal(audit.available, true);
  assert.equal(audit.exactMaterials.length, 0);
  assert.deepEqual(audit.materialEntries[0].missingResources, ["txDamageMask"]);
  assert.ok(audit.warnings[0].includes("txDamageMask"));
});

test("audits installed Abarth damage nodes and material resources", async (context) => {
  let bytes;
  try { bytes = await readFile(assettoPath("content/cars/abarth500/abarth500.kn5")); }
  catch { context.skip("The installed Abarth damage fixture is unavailable"); return; }
  const model = parseKn5(bytes), audit = auditNativeDamagePreview(model.root, model.materials);
  assert.deepEqual(audit.groups.map((group) => group.selected.length), [2, 1, 2, 2, 2]);
  assert.equal(audit.roots.every((entry) => entry.active === false), true);
  assert.equal(audit.materialEntries.length, 5);
  assert.equal(audit.exactMaterials.length, 1);
  assert.deepEqual(audit.warnings, []);
});
